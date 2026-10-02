#include "zoom_bot/audio_segmenter.hpp"

#include <stdexcept>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace zoom_bot {
namespace {
std::string clock_label(int64_t milliseconds) {
  const auto seconds = milliseconds / 1000;
  std::ostringstream label;
  label << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
        << std::setw(2) << seconds / 60 % 60 << ':' << std::setw(2) << seconds % 60;
  return label.str();
}
}  // namespace
AudioSegmenter::AudioSegmenter(std::string participant_session_id, std::string display_name,
                               uint32_t user_id, int rate, int channels,
                               EventJournal& events, WavWriter& wav,
                               RecordingTimeline& recordings,
                               uint64_t& utterance_number, uint64_t& chunk_number)
    : participant_id_(std::move(participant_session_id)), display_name_(std::move(display_name)),
      user_id_(user_id), rate_(rate),
      channels_(channels), events_(events), wav_(wav),
      recordings_(recordings),
      utterance_number_(utterance_number), chunk_number_(chunk_number) {}

nlohmann::json AudioSegmenter::identity() const {
  return {{"participant_session_id", participant_id_}, {"user_id", user_id_},
          {"utterance_id", utterance_id_}, {"display_name", display_name_}};
}

nlohmann::json AudioSegmenter::recording_identity(int64_t capture_ms) const {
  const auto* span = recordings_.at(capture_ms);
  if (!span || span->sequence == 0)
    return {{"recording_id", nullptr}, {"recording_sequence", nullptr}, {"recording_time_ms", nullptr},
            {"timestamp_origin", "wall_clock_gmt9"}, {"clock_timezone", "UTC+09:00"}};
  return {{"recording_id", span->id()}, {"recording_sequence", span->sequence},
          {"recording_start_unix_ms", span->recording_start_unix_ms},
          {"timestamp_origin", span->origin_source},
          {"recording_time_ms", span->offset_ms + capture_ms - span->start_ms}};
}

void AudioSegmenter::consume(AudioFrame frame, bool voiced) {
  if (frame.samples.empty() || frame.samples.size() > static_cast<size_t>(rate_ / 100 * channels_) ||
      frame.samples.size() % channels_ != 0 ||
      (!pre_roll_.empty() && frame.start_ms != end_ms_))
    throw std::invalid_argument("Segmenter requires consecutive frames of at most 10 ms");
  const auto duration_ms = (frame.samples.size() / channels_ * 1000 + rate_ - 1) / rate_;
  end_ms_ = frame.start_ms + static_cast<int64_t>(duration_ms);
  if (voiced) {
    last_voice_end_ms_ = end_ms_;
    if (!speaking_) {
      speaking_ = true;
      next_cut_ms_ = frame.start_ms + 30000;
      utterance_id_ = participant_id_ + "-utterance-" + std::to_string(++utterance_number_);
      chunk_.assign(pre_roll_.begin(), pre_roll_.end());
      auto event = identity();
      event.update(recording_identity(frame.start_ms));
      event["speech_start_ms"] = event["recording_time_ms"];
      event["capture_speech_start_ms"] = frame.start_ms;
      event["speech_start_unix_ms"] = recordings_.wall_ms(frame.start_ms);
      event["detector"] = "webrtc-vad";
      events_.append("speech_on", frame.start_ms, std::move(event));
    }
  }
  if (speaking_) {
    chunk_.push_back(frame);
    if (!voiced && end_ms_ - last_voice_end_ms_ >= 400) {
      finish("silence");
    } else if (voiced && end_ms_ >= next_cut_ms_) {
      std::vector<AudioFrame> overlap(chunk_.end() - 20, chunk_.end());
      publish("max_duration");
      chunk_ = std::move(overlap);
      overlap_ms_ = 200;
      next_cut_ms_ += 30000;
    }
  }
  pre_roll_.push_back(std::move(frame));
  if (pre_roll_.size() > 20) pre_roll_.pop_front();
}

void AudioSegmenter::finish(const std::string& reason, int64_t detected_ms) {
  if (speaking_) {
    // Overlap alone after a forced cut must not become a duplicate-only chunk.
    if (chunk_.size() > static_cast<size_t>(overlap_ms_ / 10)) publish(reason);
    auto event = identity();
    event.update(recording_identity(last_voice_end_ms_ - 1));
    if (!event["recording_time_ms"].is_null())
      event["recording_time_ms"] = event["recording_time_ms"].get<int64_t>() + 1;
    event["speech_end_ms"] = event["recording_time_ms"];
    event["capture_speech_end_ms"] = last_voice_end_ms_;
    event["speech_end_unix_ms"] = recordings_.wall_ms(last_voice_end_ms_);
    event["reason"] = reason;
    events_.append("speech_off", detected_ms < 0 ? end_ms_ : detected_ms, std::move(event));
  }
  speaking_ = false;
  overlap_ms_ = 0;
  chunk_.clear();
  pre_roll_.clear();
}

void AudioSegmenter::recording_changed(const std::string& reason, int64_t at_ms) {
  std::vector<AudioFrame> pending;
  const auto keep_from = chunk_.empty() ? at_ms : std::max(at_ms, chunk_.front().start_ms + overlap_ms_);
  for (const auto& frame : chunk_) {
    const auto frames = static_cast<int64_t>(frame.samples.size() / channels_);
    const auto used = std::clamp((keep_from - frame.start_ms) * rate_ / 1000, int64_t{0}, frames);
    if (used < frames)
      pending.push_back({frame.start_ms + used * 1000 / rate_,
                         std::vector<int16_t>(frame.samples.begin() + used * channels_, frame.samples.end())});
  }
  if (speaking_ && chunk_.size() > static_cast<size_t>(overlap_ms_ / 10)) publish(reason, at_ms);
  chunk_ = std::move(pending);
  pre_roll_.clear();
  overlap_ms_ = 0;
  next_cut_ms_ = at_ms + 30000;
  // A cloud file boundary does not mean that the person stopped speaking.
}

void AudioSegmenter::publish(const std::string& reason, int64_t limit_ms) {
  for (const auto& span : recordings_.spans()) {
    const auto start = std::max(chunk_.front().start_ms, span.start_ms);
    const auto end = std::min({end_ms_, span.end_ms, limit_ms});
    if (start >= end) continue;
    std::vector<int16_t> samples;
    for (const auto& frame : chunk_) {
      const auto frames = static_cast<int64_t>(frame.samples.size() / channels_);
      const auto first = std::clamp((start - frame.start_ms) * rate_ / 1000, int64_t{0}, frames);
      const auto last = std::clamp((end - frame.start_ms) * rate_ / 1000, int64_t{0}, frames);
      samples.insert(samples.end(), frame.samples.begin() + first * channels_, frame.samples.begin() + last * channels_);
    }
    if (samples.empty()) continue;
    auto metadata = identity();
    metadata.update(recording_identity(start));
    metadata.erase("recording_time_ms");
    const auto number = ++chunk_number_;
    const auto file_number = wav_.next_sequence();
    const auto id = events_.session_id() + "-" + participant_id_ + "-chunk-" + std::to_string(number);
    const auto start_ms = span.offset_ms + start - span.start_ms;
    const auto end_ms = span.offset_ms + end - span.start_ms;
    const auto label = WavWriter::speaker_label(display_name_);
    const auto display_start = span.sequence == 0 ? (start_ms + 9 * 3600000) % 86400000 : start_ms;
    const auto display_end = span.sequence == 0 ? (end_ms + 9 * 3600000) % 86400000 : end_ms;
    const auto relative = span.id() + "__" + label + "__" + clock_label(display_start) + "-" +
                          clock_label(display_end) + "__chunk-" + std::to_string(file_number) + ".wav";
    const auto overlap = std::max<int64_t>(0, std::min(end, chunk_.front().start_ms + overlap_ms_) - start);
    metadata.update({{"chunk_id", id}, {"sequence", number}, {"file_sequence", file_number},
                     {"start_ms", start_ms}, {"end_ms", end_ms},
                     {"capture_start_ms", start}, {"capture_end_ms", end}, {"speaker_label", label},
                     {"start_unix_ms", recordings_.wall_ms(start)}, {"end_unix_ms", recordings_.wall_ms(end)},
                     {"overlap_ms", overlap}, {"cut_reason", reason}, {"sample_rate", rate_}, {"channels", channels_},
                     {"sample_format", "pcm_s16le"}, {"sample_frames", samples.size() / channels_},
                     {"wav_path", wav_.write(relative, rate_, channels_, samples)}});
    events_.append("audio.chunk_ready", end, std::move(metadata));
  }
  chunk_.clear();
  overlap_ms_ = 0;
}
}  // namespace zoom_bot

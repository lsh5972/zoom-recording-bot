#include "zoom_bot/audio_segmenter.hpp"

#include <stdexcept>
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
AudioSegmenter::AudioSegmenter(std::string participant_session_id, uint32_t user_id, int rate, int channels,
                               EventJournal& events, WavWriter& wav,
                               uint64_t& utterance_number, uint64_t& chunk_number)
    : participant_id_(std::move(participant_session_id)), user_id_(user_id), rate_(rate),
      channels_(channels), events_(events), wav_(wav),
      utterance_number_(utterance_number), chunk_number_(chunk_number) {}

nlohmann::json AudioSegmenter::identity() const {
  return {{"participant_session_id", participant_id_}, {"user_id", user_id_},
          {"utterance_id", utterance_id_}};
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
      event["speech_start_ms"] = frame.start_ms;
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
    event["speech_end_ms"] = last_voice_end_ms_;
    event["reason"] = reason;
    events_.append("speech_off", detected_ms < 0 ? end_ms_ : detected_ms, std::move(event));
  }
  speaking_ = false;
  overlap_ms_ = 0;
  chunk_.clear();
  pre_roll_.clear();
}

void AudioSegmenter::publish(const std::string& reason) {
  std::vector<int16_t> samples;
  samples.reserve(chunk_.size() * rate_ / 100 * channels_);
  for (const auto& frame : chunk_) samples.insert(samples.end(), frame.samples.begin(), frame.samples.end());
  auto metadata = identity();
  const auto number = ++chunk_number_;
  const auto id = events_.session_id() + "-" + participant_id_ + "-chunk-" + std::to_string(number);
  const auto relative = participant_id_ + "__" + clock_label(chunk_.front().start_ms) + "-" +
                        clock_label(end_ms_) + "__chunk-" + std::to_string(number) + ".wav";
  metadata.update({{"chunk_id", id}, {"sequence", number}, {"start_ms", chunk_.front().start_ms},
                   {"end_ms", end_ms_}, {"overlap_ms", overlap_ms_},
                   {"cut_reason", reason}, {"sample_rate", rate_}, {"channels", channels_},
                   {"sample_format", "pcm_s16le"}, {"sample_frames", samples.size() / channels_},
                   {"wav_path", wav_.write(relative, rate_, channels_, samples)}});
  events_.append("audio.chunk_ready", end_ms_, std::move(metadata));
  chunk_.clear();
  overlap_ms_ = 0;
}
}  // namespace zoom_bot

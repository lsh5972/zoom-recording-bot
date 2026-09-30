#include "zoom_bot/audio_pipeline.hpp"

#include <cstdlib>
#include <stdexcept>

namespace zoom_bot {
AudioPipeline::AudioPipeline(EventJournal& events, WavWriter& wav) : events_(events), wav_(wav) {}

AudioPipeline::Stream::Stream(const std::string& participant_id, const PcmPacket& packet,
                             EventJournal& events, WavWriter& wav,
                             uint64_t& utterance_number, uint64_t& chunk_number)
    : rate(packet.rate), channels(packet.channels), origin_ms(packet.start_ms), buffer_ms(packet.start_ms),
      detector(rate), segmenter(participant_id, packet.user_id, rate, channels, events, wav,
                                utterance_number, chunk_number) {}

int64_t AudioPipeline::Stream::expected_ms() const {
  return origin_ms + static_cast<int64_t>(received_samples * 1000 / rate);
}

void AudioPipeline::Stream::reset(int64_t start_ms) {
  origin_ms = buffer_ms = start_ms;
  received_samples = 0;
  pending.clear();
  detector.reset();
  idle = false;
}

void AudioPipeline::Stream::drain() {
  if (pending.empty()) return;
  auto padded = pending;
  padded.resize(static_cast<size_t>(rate / 100 * channels), 0);
  const bool voiced = detector.voiced(padded, channels);
  segmenter.consume({buffer_ms, std::move(pending)}, voiced);
  pending.clear();
}

void AudioPipeline::joined(uint32_t user_id, const std::string& display_name, int64_t at_ms) {
  if (participants_.count(user_id)) left(user_id, at_ms);
  const auto id = "user-" + std::to_string(user_id) + "-join-" + std::to_string(++generations_[user_id]);
  participants_.emplace(user_id, Participant{id, 0, 0, nullptr});
  events_.append("participant.joined", at_ms,
                 {{"user_id", user_id}, {"participant_session_id", id}, {"display_name", display_name}});
}

void AudioPipeline::left(uint32_t user_id, int64_t at_ms) {
  auto found = participants_.find(user_id);
  if (found == participants_.end()) return;
  if (found->second.stream) {
    found->second.stream->drain();
    found->second.stream->segmenter.finish("participant_left", at_ms);
  }
  events_.append("participant.left", at_ms,
                 {{"user_id", user_id}, {"participant_session_id", found->second.id}});
  participants_.erase(found);
}

void AudioPipeline::consume(PcmPacket packet) {
  if (packet.start_ms < 0 || (packet.channels != 1 && packet.channels != 2) || packet.samples.empty() ||
      packet.samples.size() % packet.channels != 0 ||
      (packet.rate != 8000 && packet.rate != 16000 && packet.rate != 32000 && packet.rate != 48000))
    throw std::invalid_argument("Unsupported or malformed PCM packet");
  if (!participants_.count(packet.user_id)) {
    joined(packet.user_id, "", packet.start_ms);
    events_.append("participant.discovered_from_audio", packet.start_ms, {{"user_id", packet.user_id}});
  }
  auto& participant = participants_.at(packet.user_id);
  if (participant.stream && (participant.stream->rate != packet.rate || participant.stream->channels != packet.channels)) {
    participant.stream->drain();
    participant.stream->segmenter.finish("format_changed");
    participant.stream.reset();
  }
  if (!participant.stream) {
    participant.stream = std::make_unique<Stream>(participant.id, packet, events_, wav_,
                                                participant.utterance_number, participant.chunk_number);
    events_.append("audio.format", packet.start_ms,
                   {{"participant_session_id", participant.id}, {"user_id", packet.user_id},
                    {"sample_rate", packet.rate}, {"channels", packet.channels}});
  }
  auto& stream = *participant.stream;
  const auto expected = stream.expected_ms();
  if (std::llabs(packet.start_ms - expected) > 1) {
    if (packet.start_ms < expected) {
      events_.append("audio.out_of_order", packet.start_ms,
                     {{"user_id", packet.user_id}, {"expected_ms", expected}});
      throw std::runtime_error("PCM timestamp moved backwards");
    }
    stream.drain();
    stream.segmenter.finish("audio_gap");
    events_.append("audio.gap", packet.start_ms,
                   {{"user_id", packet.user_id}, {"participant_session_id", participant.id},
                    {"start_ms", expected}, {"end_ms", packet.start_ms}});
    stream.reset(packet.start_ms);
  } else if (stream.idle) {
    stream.reset(packet.start_ms);
  }
  stream.received_samples += packet.samples.size() / packet.channels;
  stream.pending.insert(stream.pending.end(), packet.samples.begin(), packet.samples.end());
  const auto frame_size = static_cast<size_t>(stream.rate / 100 * stream.channels);
  size_t used = 0;
  while (stream.pending.size() - used >= frame_size) {
    std::vector<int16_t> frame(stream.pending.begin() + used, stream.pending.begin() + used + frame_size);
    const bool voiced = stream.detector.voiced(frame, stream.channels);
    stream.segmenter.consume({stream.buffer_ms, std::move(frame)}, voiced);
    stream.buffer_ms += 10;
    used += frame_size;
  }
  stream.pending.erase(stream.pending.begin(), stream.pending.begin() + used);
}

void AudioPipeline::advance(int64_t now_ms) {
  for (auto& [user_id, participant] : participants_) {
    (void)user_id;
    if (participant.stream && !participant.stream->idle && now_ms - participant.stream->expected_ms() >= 400) {
      auto& stream = *participant.stream;
      stream.drain();
      stream.segmenter.finish("inactivity", now_ms);
      stream.idle = true;
    }
  }
}

void AudioPipeline::finish(const std::string& reason, int64_t at_ms) {
  for (auto& [user_id, participant] : participants_) {
    (void)user_id;
    if (participant.stream) {
      participant.stream->drain();
      participant.stream->segmenter.finish(reason, at_ms);
    }
  }
  events_.append("capture.stopped", at_ms, {{"reason", reason}});
  participants_.clear();
}
}  // namespace zoom_bot

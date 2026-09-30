#pragma once

#include "zoom_bot/audio_segmenter.hpp"
#include "zoom_bot/speech_detector.hpp"
#include <map>
#include <memory>

namespace zoom_bot {
struct PcmPacket {
  uint32_t user_id;
  int rate, channels;
  int64_t start_ms;  // Already normalized to the meeting's shared capture timeline.
  std::vector<int16_t> samples;
};

// Run on the capture consumer, not inside SDK callbacks.
class AudioPipeline {
 public:
  AudioPipeline(EventJournal& events, WavWriter& wav);
  void joined(uint32_t user_id, const std::string& display_name, int64_t at_ms);
  void left(uint32_t user_id, int64_t at_ms);
  void consume(PcmPacket packet);
  void advance(int64_t now_ms);
  void pause(const std::string& reason, int64_t at_ms);
  void finish(const std::string& reason, int64_t at_ms);

 private:
  struct Stream {
    Stream(const std::string& participant_id, const PcmPacket& packet, EventJournal& events, WavWriter& wav,
           uint64_t& utterance_number, uint64_t& chunk_number);
    int rate, channels;
    int64_t origin_ms, buffer_ms;
    uint64_t received_samples = 0;
    std::vector<int16_t> pending;
    SpeechDetector detector;
    AudioSegmenter segmenter;
    bool idle = false;
    int64_t expected_ms() const;
    void drain();
    void reset(int64_t start_ms);
  };
  struct Participant {
    std::string id;
    uint64_t utterance_number = 0, chunk_number = 0;
    std::unique_ptr<Stream> stream;
  };
  EventJournal& events_;
  WavWriter& wav_;
  std::map<uint32_t, Participant> participants_;
  std::map<uint32_t, uint64_t> generations_;
};
}  // namespace zoom_bot

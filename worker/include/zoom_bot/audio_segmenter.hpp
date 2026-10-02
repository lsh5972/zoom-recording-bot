#pragma once

#include "zoom_bot/event_journal.hpp"
#include "zoom_bot/recording_timeline.hpp"
#include "zoom_bot/wav_writer.hpp"
#include <deque>

namespace zoom_bot {
struct AudioFrame {
  int64_t start_ms;
  std::vector<int16_t> samples;
};

class AudioSegmenter {
 public:
  AudioSegmenter(std::string participant_session_id, std::string display_name, uint32_t user_id, int rate, int channels,
                 EventJournal& events, WavWriter& wav, RecordingTimeline& recordings,
                 uint64_t& utterance_number, uint64_t& chunk_number);
  void consume(AudioFrame frame, bool voiced);
  // Covers SDK silence with no PCM callbacks, departure and permission loss.
  void finish(const std::string& reason, int64_t detected_ms = -1);
  void recording_changed(const std::string& reason, int64_t at_ms);
  void renamed(const std::string& display_name) { display_name_ = display_name; }
  int64_t end_ms() const { return end_ms_; }

 private:
  void publish(const std::string& reason, int64_t limit_ms = INT64_MAX);
  nlohmann::json identity() const;
  nlohmann::json recording_identity(int64_t capture_ms) const;
  std::string participant_id_, display_name_;
  uint32_t user_id_;
  int rate_, channels_;
  EventJournal& events_;
  WavWriter& wav_;
  RecordingTimeline& recordings_;
  std::deque<AudioFrame> pre_roll_;
  std::vector<AudioFrame> chunk_;
  bool speaking_ = false;
  uint64_t& utterance_number_;
  uint64_t& chunk_number_;
  std::string utterance_id_;
  int64_t last_voice_end_ms_ = 0, end_ms_ = 0;
  int64_t next_cut_ms_ = 0;
  int overlap_ms_ = 0;
};
}  // namespace zoom_bot

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zoom_bot {
struct RecordingSpan {
  uint64_t sequence;
  int64_t start_ms, end_ms, offset_ms, recording_start_unix_ms;
  std::string origin_source;
  std::string id() const { return sequence == 0 ? "unrecorded" : "recording-" + std::to_string(sequence); }
};

// Capture time stays monotonic. These spans map it to each cloud file's playback time.
class RecordingTimeline {
 public:
  bool change(int status, int64_t at_ms, int64_t first_origin_ms, int64_t admission_unix_ms);
  const std::vector<RecordingSpan>& spans() const { return spans_; }
  const RecordingSpan* at(int64_t capture_ms) const;
  int64_t wall_ms(int64_t capture_ms) const { return admission_unix_ms_ + capture_ms; }

 private:
  int status_ = -1;
  uint64_t sequence_ = 0;
  int64_t admission_unix_ms_ = 0;
  std::vector<RecordingSpan> spans_;
};
}  // namespace zoom_bot

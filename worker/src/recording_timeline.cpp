#include "zoom_bot/recording_timeline.hpp"

#include <stdexcept>
#include <algorithm>

namespace zoom_bot {
bool RecordingTimeline::change(int status, int64_t at_ms, int64_t first_origin_ms,
                               int64_t admission_unix_ms) {
  // SDK RecordingStatus: start=0, stop=1, disk full=2, pause=3, connecting=4, fail=5.
  if (spans_.empty()) {
    admission_unix_ms_ = admission_unix_ms;
    spans_.push_back({0, 0, INT64_MAX, admission_unix_ms, 0, "wall_clock_gmt9"});
  }
  if ((status == 4 && status_ != -1) || status < 0 || status > 5 || status == status_) return false;
  if (status_ == -1 && status == 0) spans_.clear();  // Admission snapshot applies to the whole first file.
  if (!spans_.empty() && spans_.back().end_ms == INT64_MAX) {
    if (at_ms < spans_.back().start_ms) throw std::runtime_error("Cloud recording status moved backwards");
    spans_.back().end_ms = at_ms;
  }
  if (status == 0) {
    if (status_ == 3 && sequence_ > 0) {
      const auto previous = *std::find_if(spans_.rbegin(), spans_.rend(),
                                        [](const auto& span) { return span.sequence > 0; });
      spans_.push_back({sequence_, at_ms, INT64_MAX,
                        previous.offset_ms + previous.end_ms - previous.start_ms,
                        previous.recording_start_unix_ms, previous.origin_source});
    } else {
      const auto start = status_ == -1 ? first_origin_ms : at_ms;
      if (start > at_ms) throw std::runtime_error("Meeting start_time is later than bot admission");
      spans_.push_back({++sequence_, start, INT64_MAX, 0, admission_unix_ms + start,
                        status_ == -1 ? "meeting_api.start_time" : "sdk.cloud_recording_callback"});
    }
  }
  if (status != 0) spans_.push_back({0, at_ms, INT64_MAX, admission_unix_ms + at_ms, 0, "wall_clock_gmt9"});
  status_ = status;
  return true;
}

const RecordingSpan* RecordingTimeline::at(int64_t capture_ms) const {
  for (auto it = spans_.rbegin(); it != spans_.rend(); ++it)
    if (capture_ms >= it->start_ms && capture_ms < it->end_ms) return &*it;
  return nullptr;
}
}  // namespace zoom_bot

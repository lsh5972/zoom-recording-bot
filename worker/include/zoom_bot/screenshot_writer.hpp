#pragma once

#include "zoom_bot/recording_timeline.hpp"
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

namespace zoom_bot {
struct ScreenshotFrame {
  uint32_t user_id = 0, share_source_id = 0;
  int width = 0, height = 0, rotation = 0;
  bool limited_range = true;
  uint64_t source_ms = 0;
  std::shared_ptr<const std::vector<unsigned char>> i420;
};

class ScreenshotWriter {
 public:
  explicit ScreenshotWriter(std::filesystem::path output) : output_(std::move(output)) {}
  nlohmann::json write(const ScreenshotFrame& frame, int64_t capture_ms, const RecordingTimeline& recordings);

 private:
  std::filesystem::path output_;
  uint64_t sequence_ = 0;
  ScreenshotFrame encoded_frame_;
  std::vector<unsigned char> encoded_jpeg_;
};
}  // namespace zoom_bot

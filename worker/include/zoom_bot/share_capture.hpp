#pragma once

#include "zoom_bot/capture_runtime.hpp"
#include "zoom_bot/sdk_headers.hpp"
#include <optional>

namespace zoom_bot {
// Owns the SDK share renderer and copies its latest frame between samples.
class ShareCapture final : public ZOOMSDK::IZoomSDKRendererDelegate {
 public:
  ShareCapture(CaptureRuntime& capture, int interval_seconds);
  ~ShareCapture();
  void refresh(ZOOMSDK::IMeetingShareController* controller);
  void changed(uint32_t user_id, uint32_t source_id, ZOOMSDK::SharingStatus status, ZOOMSDK::ShareType type);
  void suspend();  // Immediately reject SDK frames; cleanup stays on the SDK main thread.
  void stop();
  void tick(int64_t at_ms);
  void onRendererBeDestroyed() override;
  void onRawDataFrameReceived(YUVRawDataI420* data) override;
  void onRawDataStatusChanged(RawDataStatus status) override;
  void onShareCursorDataReceived(ZoomSDKShareCursorData info) override;

 private:
  void select(uint32_t user_id, uint32_t source_id);
  void operation(ZOOMSDK::SDKError result, const char* name);
  CaptureRuntime& capture_;
  const int64_t interval_ms_;
  std::atomic<ZOOMSDK::IZoomSDKRenderer*> renderer_{nullptr};
  std::mutex mutex_;
  bool receiving_ = false;
  uint32_t user_id_ = 0, source_id_ = 0;
  int64_t next_ms_ = 0;
  std::optional<ScreenshotFrame> latest_;
  nlohmann::json previous_sources_;
};
}  // namespace zoom_bot

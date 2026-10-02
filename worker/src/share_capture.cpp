#include "zoom_bot/share_capture.hpp"

#include <cstring>
#include <stdexcept>

namespace zoom_bot {
using namespace ZOOMSDK;
namespace {
bool visible(SharingStatus status, ShareType type) {
  return type != SHARE_TYPE_COMPUTER_AUDIO &&
         (status == Sharing_Other_Share_Begin || status == Sharing_View_Other_Sharing ||
          status == Sharing_Resume || status == Sharing_Pause);
}
}

ShareCapture::ShareCapture(CaptureRuntime& capture, int interval_seconds)
    : capture_(capture), interval_ms_(static_cast<int64_t>(interval_seconds) * 1000) {
  if (interval_seconds <= 0) throw std::invalid_argument("Invalid screenshot interval");
}
ShareCapture::~ShareCapture() { stop(); }

void ShareCapture::operation(SDKError result, const char* name) {
  capture_.event("sdk.operation", {{"operation", name}, {"result", static_cast<int>(result)}});
  if (result != SDKERR_SUCCESS) throw std::runtime_error("SDK share operation failed");
}

void ShareCapture::suspend() {
  std::lock_guard<std::mutex> lock(mutex_);
  receiving_ = false;
  latest_.reset();
}

void ShareCapture::stop() {
  suspend();
  auto* renderer = renderer_.exchange(nullptr);
  if (renderer) {
    capture_.event("sdk.operation", {{"operation", "unsubscribe_share"}, {"result", static_cast<int>(renderer->unSubscribe())}});
    capture_.event("sdk.operation", {{"operation", "destroy_share_renderer"}, {"result", static_cast<int>(destroyRenderer(renderer))}});
  }
  std::lock_guard<std::mutex> lock(mutex_);
  user_id_ = source_id_ = 0;
  next_ms_ = 0;
}

void ShareCapture::select(uint32_t user_id, uint32_t source_id) {
  if (!source_id || (renderer_ && source_id_ == source_id)) return;
  stop();
  IZoomSDKRenderer* renderer = nullptr;
  operation(createRenderer(&renderer, this), "create_share_renderer");
  renderer_ = renderer;
  if (!renderer) throw std::runtime_error("SDK share renderer unavailable");
  operation(renderer->setRawDataResolution(ZoomSDKResolution_360P), "set_share_resolution_360p");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    user_id_ = user_id;
    source_id_ = source_id;
    receiving_ = true;
  }
  operation(renderer->subscribe(source_id, RAW_DATA_TYPE_SHARE), "subscribe_share");
  capture_.event("sdk.event_coverage", {{"interface", "IZoomSDKRendererDelegate"}, {"registered", true},
                                       {"callbacks", {"onRendererBeDestroyed", "onRawDataFrameReceived",
                                                      "onRawDataStatusChanged", "onShareCursorDataReceived"}}});
  capture_.event("share.capture_started", {{"user_id", user_id}, {"share_source_id", source_id},
                                           {"interval_seconds", interval_ms_ / 1000}, {"width", 640}, {"height", 360}});
}

void ShareCapture::changed(uint32_t user_id, uint32_t source_id, SharingStatus status, ShareType type) {
  if (visible(status, type)) select(user_id, source_id);
  else if ((status == Sharing_Other_Share_End || type == SHARE_TYPE_COMPUTER_AUDIO) &&
           (source_id == source_id_ || (!source_id && user_id == user_id_))) stop();
}

void ShareCapture::refresh(IMeetingShareController* controller) {
  if (!controller) return;
  auto* users = controller->GetViewableSharingUserList();
  if (!users) return;
  uint32_t user_id = 0, source_id = 0;
  auto snapshot = nlohmann::json::array();
  bool current_visible = false;
  for (int i = 0; i < users->GetCount(); ++i) {
    auto* sources = controller->GetSharingSourceInfoList(users->GetItem(i));
    if (!sources) continue;
    for (int j = 0; j < sources->GetCount(); ++j) {
      const auto info = sources->GetItem(j);
      snapshot.push_back({{"user_id", info.userid}, {"share_source_id", info.shareSourceID},
                          {"status", static_cast<int>(info.status)}, {"content_type", static_cast<int>(info.contentType)}});
      if (!visible(info.status, info.contentType) || !info.shareSourceID) continue;
      if (renderer_ && info.shareSourceID == source_id_) current_visible = true;
      if (!source_id) { user_id = info.userid; source_id = info.shareSourceID; }
    }
  }
  if (snapshot != previous_sources_) {
    capture_.event("share.sources", {{"sources", snapshot}, {"sharing_user_count", users->GetCount()}});
    previous_sources_ = std::move(snapshot);
  }
  if (current_visible) return;
  if (source_id) select(user_id, source_id);
  else if (users->GetCount() == 0) stop();
}

void ShareCapture::tick(int64_t at_ms) {
  ScreenshotFrame frame;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!receiving_ || !latest_ || at_ms < next_ms_) return;
    frame = *latest_;
    next_ms_ = at_ms + interval_ms_;  // No catch-up burst after a delayed main loop.
  }
  capture_.screenshot(std::move(frame), at_ms);
}

void ShareCapture::onRawDataFrameReceived(YUVRawDataI420* data) {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!receiving_ || !data) return;
    const auto width = data->GetStreamWidth(), height = data->GetStreamHeight();
    const auto y_size = static_cast<size_t>(width) * height;
    const auto uv_size = static_cast<size_t>((width + 1) / 2) * ((height + 1) / 2);
    if (!width || !height || width > 8192 || height > 8192 || y_size + 2 * uv_size > 32 * 1024 * 1024 ||
        data->GetBufferLen() < y_size + 2 * uv_size || !data->GetYBuffer() || !data->GetUBuffer() || !data->GetVBuffer())
      throw std::runtime_error("Invalid SDK share frame");
    if (!latest_) latest_.emplace();
    auto& frame = *latest_;
    frame.user_id = user_id_;
    frame.share_source_id = source_id_;
    frame.width = width; frame.height = height;
    frame.rotation = data->GetRotation(); frame.limited_range = data->IsLimitedI420();
    frame.source_ms = data->GetTimeStamp();
    frame.i420.resize(y_size + 2 * uv_size);
    std::memcpy(frame.i420.data(), data->GetYBuffer(), y_size);
    std::memcpy(frame.i420.data() + y_size, data->GetUBuffer(), uv_size);
    std::memcpy(frame.i420.data() + y_size + uv_size, data->GetVBuffer(), uv_size);
    capture_.event("sdk.raw_share.frame", {{"source_id", data->GetSourceID()}, {"source_timestamp_ms", data->GetTimeStamp()},
                                           {"width", data->GetStreamWidth()}, {"height", data->GetStreamHeight()},
                                           {"rotation", data->GetRotation()}, {"bytes", data->GetBufferLen()}});
  } catch (...) { capture_.fail(); }
}
void ShareCapture::onRendererBeDestroyed() {
  renderer_ = nullptr;
  suspend();
  capture_.event("sdk.callback", {{"interface", "IZoomSDKRendererDelegate"}, {"callback", "onRendererBeDestroyed"}});
}
void ShareCapture::onRawDataStatusChanged(RawDataStatus status) {
  if (status == RawData_Off) { std::lock_guard<std::mutex> lock(mutex_); latest_.reset(); }
  capture_.event("sdk.callback", {{"interface", "IZoomSDKRendererDelegate"}, {"callback", "onRawDataStatusChanged"},
                                   {"arguments", {{"status", static_cast<int>(status)}}}});
}
void ShareCapture::onShareCursorDataReceived(ZoomSDKShareCursorData info) {
  capture_.event("sdk.callback", {{"interface", "IZoomSDKRendererDelegate"}, {"callback", "onShareCursorDataReceived"},
                                   {"arguments", {{"source_id", info.source_id}, {"x", info.x}, {"y", info.y}}}});
}
}  // namespace zoom_bot

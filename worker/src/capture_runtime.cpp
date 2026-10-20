#include "zoom_bot/capture_runtime.hpp"

#include <algorithm>
#include <limits>
#include <iostream>
#include <map>
#include <optional>

namespace zoom_bot {
namespace {
int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
size_t json_storage_bytes(const nlohmann::json& value) {
  using Json = nlohmann::json;
  size_t bytes = sizeof(Json);
  if (value.is_string())
    bytes += sizeof(Json::string_t) + value.get_ref<const Json::string_t&>().capacity() + 1;
  else if (value.is_object()) {
    bytes += sizeof(Json::object_t);
    for (auto entry = value.begin(); entry != value.end(); ++entry)
      bytes += sizeof(Json::object_t::value_type) + 4 * sizeof(void*) + entry.key().capacity() + 1 +
               json_storage_bytes(entry.value()) - sizeof(Json);
  } else if (value.is_array()) {
    bytes += sizeof(Json::array_t) + value.get_ref<const Json::array_t&>().capacity() * sizeof(Json);
    for (const auto& entry : value) bytes += json_storage_bytes(entry) - sizeof(Json);
  }
  return bytes;
}
}

CaptureRuntime::CaptureRuntime(const std::filesystem::path& output, const std::string& session_id,
                               size_t capacity, int64_t meeting_start_unix_ms)
    : events_(output, session_id), chunks_(output, session_id, "chunks.jsonl"),
      wav_(output), screenshots_(output), audio_(chunks_, wav_, recordings_), capacity_(capacity),
      meeting_start_unix_ms_(meeting_start_unix_ms),
      thread_([this] { consume(); }) {}

CaptureRuntime::~CaptureRuntime() { close(); }

void CaptureRuntime::admitted() {
  int64_t unset = -1;
  if (origin_ms_.compare_exchange_strong(unset, steady_ms()))
    admission_unix_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t CaptureRuntime::now_ms() const {
  const auto origin = origin_ms_.load();
  return origin < 0 ? 0 : steady_ms() - origin;
}

void CaptureRuntime::fail() noexcept { failed_ = true; ready_.notify_one(); }

void CaptureRuntime::push(Item item) noexcept {
  try {
    item.bytes = sizeof(Item) + item.text.capacity() + 1 + json_storage_bytes(item.data) - sizeof(nlohmann::json) +
                 item.packet.samples.capacity() * sizeof(int16_t) + (item.frame.i420 ? item.frame.i420->capacity() : 0);
    bool wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_ || failed_) return;
      if (queue_.size() >= 20000 || item.bytes > capacity_ - std::min(bytes_, capacity_)) {
        fail();  // Fail closed; SDK main loop will stop capture rather than silently lose audio.
        return;
      }
      wake = queue_.empty();
      bytes_ += item.bytes;
      queue_.push_back(std::move(item));
    }
    if (wake) ready_.notify_one();
  } catch (...) { fail(); }
}

void CaptureRuntime::sdk_callback(const char* interface, const char* callback, nlohmann::json arguments) noexcept {
  try {
    push({Kind::Callback, now_ms(), {},
          {{"interface", interface}, {"callback", callback}, {"arguments", std::move(arguments)}}});
  } catch (...) { fail(); }
}
void CaptureRuntime::joined(uint32_t user_id, std::string name) noexcept {
  Item item{Kind::Joined, now_ms(), std::move(name), {}};
  item.packet.user_id = user_id;
  push(std::move(item));
}
void CaptureRuntime::left(uint32_t user_id) noexcept {
  Item item{Kind::Left, now_ms(), {}, {}};
  item.packet.user_id = user_id;
  push(std::move(item));
}
void CaptureRuntime::renamed(uint32_t user_id, std::string name) noexcept {
  Item item{Kind::Renamed, now_ms(), std::move(name), {}};
  item.packet.user_id = user_id;
  push(std::move(item));
}
void CaptureRuntime::pcm(PcmPacket packet, uint64_t source_ms) noexcept {
  Item item{Kind::Pcm, now_ms(), {}, {}};
  item.packet = std::move(packet);
  item.source_ms = source_ms;
  push(std::move(item));
}
void CaptureRuntime::pause(std::string reason) noexcept {
  push({Kind::Pause, now_ms(), std::move(reason), {}});
}

void CaptureRuntime::cloud_recording(int status) noexcept {
  if (origin_ms_ < 0) return;  // Pre-admission notifications are still kept in sdk.callback.
  push({Kind::CloudRecording, now_ms(), {}, {{"status", status}}});
}

void CaptureRuntime::screenshot(ScreenshotFrame frame, int64_t at_ms) noexcept {
  Item item{Kind::Screenshot, at_ms, {}, {}};
  item.frame = std::move(frame);
  push(std::move(item));
}

void CaptureRuntime::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  ready_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void CaptureRuntime::consume() noexcept {
  std::optional<int64_t> source_offset;
  struct SourceClock {
    int rate, channels;
    int64_t origin_ms, last_source_ms;
    uint64_t frames;
  };
  std::map<uint32_t, SourceClock> source_clocks;
  try {
    for (;;) {
      Item item{};
      bool available = false, closing;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, std::chrono::milliseconds(50), [this] { return closing_ || failed_ || !queue_.empty(); });
        if (!queue_.empty()) {
          item = std::move(queue_.front());
          queue_.pop_front();
          bytes_ -= item.bytes;
          available = true;
        }
        closing = closing_;
      }
      if (!available) {
        if (failed_) {
          std::cerr << "Capture failed: callback queue overflow or allocation failure\n";
          break;
        }
        if (closing) break;
        audio_.advance(now_ms());
        events_.sync_if_due();
        chunks_.sync_if_due();
        continue;
      }
      switch (item.kind) {
        case Kind::Screenshot:
          screenshots_.write(item.frame, item.at_ms, recordings_);
          break;
        case Kind::Callback: events_.append("sdk.callback", item.at_ms, std::move(item.data)); break;
        case Kind::Joined:
          source_clocks.erase(item.packet.user_id);
          audio_.joined(item.packet.user_id, item.text, item.at_ms);
          break;
        case Kind::Left:
          source_clocks.erase(item.packet.user_id);
          audio_.left(item.packet.user_id, item.at_ms);
          break;
        case Kind::Renamed:
          audio_.renamed(item.packet.user_id, item.text, item.at_ms);
          break;
        case Kind::Pause:
          audio_.pause(item.text, item.at_ms);
          source_clocks.clear();
          if (item.text == "reconnecting") source_offset.reset();
          break;
        case Kind::CloudRecording: {
          const auto status = item.data.at("status").get<int>();
          const auto first_origin = meeting_start_unix_ms_ > 0 ? meeting_start_unix_ms_ - admission_unix_ms_ : 0;
          if (recordings_.change(status, item.at_ms, first_origin, admission_unix_ms_)) {
            const auto reason = status == 0 ? "cloud_recording_started" :
                                status == 3 ? "cloud_recording_paused" : "cloud_recording_stopped";
            audio_.recording_changed(reason, item.at_ms);
          }
          break;
        }
        case Kind::Pcm: {
          auto& packet = item.packet;
          if (packet.rate <= 0 || packet.channels <= 0 || item.source_ms > static_cast<uint64_t>(INT64_MAX))
            throw std::runtime_error("Invalid SDK audio metadata");
          const auto duration = static_cast<int64_t>(packet.samples.size() / packet.channels * 1000 / packet.rate);
          const auto source = static_cast<int64_t>(item.source_ms);
          if (!source_offset) source_offset = std::max<int64_t>(0, item.at_ms - duration) - source;
          packet.start_ms = source + *source_offset;
          auto previous = source_clocks.find(packet.user_id);
          if (previous != source_clocks.end() && previous->second.rate == packet.rate &&
              previous->second.channels == packet.channels && source >= previous->second.last_source_ms) {
            auto& clock = previous->second;
            const auto expected = clock.origin_ms + static_cast<int64_t>(clock.frames * 1000 / clock.rate);
            // Monotonic callback timestamps may lag behind a buffered PCM burst.
            // Sample counts keep the stream continuous; actual backwards timestamps still fail.
            if (packet.start_ms <= expected + duration + 1) {
              packet.start_ms = expected;
              clock.frames += packet.samples.size() / packet.channels;
              clock.last_source_ms = source;
            } else source_clocks.erase(previous);
          } else if (previous != source_clocks.end()) source_clocks.erase(previous);
          if (!source_clocks.count(packet.user_id))
            source_clocks.emplace(packet.user_id, SourceClock{packet.rate, packet.channels, packet.start_ms,
                                                             source, packet.samples.size() / packet.channels});
          audio_.consume(std::move(packet));
          break;
        }
      }
      events_.sync_if_due();  // PCM traffic must not delay synchronization of actual SDK callbacks.
    }
    audio_.finish(failed_ ? "capture_failed" : "worker_stopped", now_ms());
    events_.sync();
    chunks_.sync();
  } catch (...) {
    failed_ = true;
    try {
      std::cerr << "Capture failed: audio, image or storage processing failed\n";
      audio_.finish("capture_failed", now_ms());
      events_.sync();
      chunks_.sync();
    } catch (...) {}  // Storage failure may also prevent final WAV publication or synchronization.
  }
}
}  // namespace zoom_bot

#include "zoom_bot/capture_runtime.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>

namespace zoom_bot {
namespace {
int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

CaptureRuntime::CaptureRuntime(const std::filesystem::path& output, const std::string& session_id,
                               size_t capacity, int64_t meeting_start_unix_ms)
    : events_(output, session_id), wav_(output), screenshots_(output), audio_(events_, wav_, recordings_), capacity_(capacity),
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
    item.bytes = sizeof(Item) + item.text.size() + item.data.dump().size() +
                 item.packet.samples.size() * sizeof(int16_t) + item.frame.i420.size();
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || failed_) return;
    if (queue_.size() >= 20000 || item.bytes > capacity_ - std::min(bytes_, capacity_)) {
      fail();  // Fail closed; SDK main loop will stop capture rather than silently lose audio.
      return;
    }
    bytes_ += item.bytes;
    queue_.push_back(std::move(item));
    ready_.notify_one();
  } catch (...) { fail(); }
}

void CaptureRuntime::event(std::string type, nlohmann::json data) noexcept {
  push({Kind::Event, now_ms(), std::move(type), std::move(data)});
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
          events_.append("capture.error", now_ms(), {{"reason", "callback_queue_failed"}});
          break;
        }
        if (closing) break;
        audio_.advance(now_ms());
        continue;
      }
      switch (item.kind) {
        case Kind::Screenshot:
          events_.append("screenshot.saved", item.at_ms, screenshots_.write(item.frame, item.at_ms, recordings_));
          break;
        case Kind::Event: events_.append(item.text, item.at_ms, std::move(item.data)); break;
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
            const auto* span = recordings_.at(item.at_ms);
            item.data["recording_id"] = span && span->sequence > 0 ? nlohmann::json(span->id()) : nlohmann::json(nullptr);
            if (span) {
              item.data["timestamp_origin"] = span->origin_source;
              item.data["recording_start_unix_ms"] = span->sequence > 0 ?
                  nlohmann::json(span->recording_start_unix_ms) : nlohmann::json(nullptr);
            }
            events_.append("cloud.recording_state", item.at_ms, std::move(item.data));
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
          events_.append("sdk.raw_audio.one_way", item.at_ms,
                         {{"user_id", packet.user_id}, {"source_timestamp_ms", source},
                          {"start_ms", packet.start_ms}, {"sample_rate", packet.rate},
                          {"channels", packet.channels}, {"sample_frames", packet.samples.size() / packet.channels}});
          audio_.consume(std::move(packet));
          break;
        }
      }
    }
    audio_.finish(failed_ ? "capture_failed" : "worker_stopped", now_ms());
  } catch (...) {
    failed_ = true;
    try {
      events_.append("capture.error", now_ms(), {{"reason", "capture_consumer_failed"}});
      audio_.finish("capture_failed", now_ms());
    } catch (...) {}  // A failed storage device cannot accept its own error event.
  }
}
}  // namespace zoom_bot

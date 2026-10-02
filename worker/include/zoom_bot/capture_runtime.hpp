#pragma once

#include "zoom_bot/audio_pipeline.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace zoom_bot {
// SDK callbacks enqueue owned values. This consumer alone touches disk and VAD.
class CaptureRuntime {
 public:
  CaptureRuntime(const std::filesystem::path& output, const std::string& session_id,
                 size_t capacity = 32 * 1024 * 1024, int64_t meeting_start_unix_ms = 0);
  ~CaptureRuntime();
  void admitted();
  int64_t now_ms() const;
  void event(std::string type, nlohmann::json data) noexcept;
  void joined(uint32_t user_id, std::string name) noexcept;
  void left(uint32_t user_id) noexcept;
  void renamed(uint32_t user_id, std::string name) noexcept;
  void pcm(PcmPacket packet, uint64_t source_ms) noexcept;
  void pause(std::string reason) noexcept;
  void cloud_recording(int status) noexcept;
  void fail() noexcept;
  bool failed() const { return failed_; }
  void close();

 private:
  enum class Kind { Event, Joined, Left, Renamed, Pcm, Pause, CloudRecording };
  struct Item {
    Kind kind;
    int64_t at_ms;
    std::string text;
    nlohmann::json data;
    PcmPacket packet{};
    uint64_t source_ms = 0;
    size_t bytes = 0;
  };
  void push(Item item) noexcept;
  void consume() noexcept;
  EventJournal events_;
  WavWriter wav_;
  RecordingTimeline recordings_;
  AudioPipeline audio_;
  const size_t capacity_;
  const int64_t meeting_start_unix_ms_;
  std::atomic<int64_t> origin_ms_{-1};
  std::atomic<int64_t> admission_unix_ms_{0};
  std::atomic<bool> failed_{false};
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> queue_;
  size_t bytes_ = 0;
  bool closing_ = false;
  std::thread thread_;
};
}  // namespace zoom_bot

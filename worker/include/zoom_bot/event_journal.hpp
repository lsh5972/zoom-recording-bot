#pragma once

#include <filesystem>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace zoom_bot {
// Single consumer owns the journal. SDK callback threads must enqueue instead.
class EventJournal {
 public:
  EventJournal(const std::filesystem::path& output, std::string session_id);
  ~EventJournal();
  EventJournal(const EventJournal&) = delete;
  EventJournal& operator=(const EventJournal&) = delete;
  void append(const std::string& type, int64_t capture_ms, nlohmann::json payload);
  void sync_if_due();
  void sync();
  const std::string& session_id() const { return session_id_; }

 private:
  int fd_;
  uint64_t sequence_ = 0;
  std::string session_id_;
  bool dirty_ = false;
  std::chrono::steady_clock::time_point next_sync_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
};
}  // namespace zoom_bot

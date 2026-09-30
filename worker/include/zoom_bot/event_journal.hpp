#pragma once

#include <filesystem>
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
  const std::string& session_id() const { return session_id_; }

 private:
  int fd_;
  uint64_t sequence_ = 0;
  std::string session_id_;
};
}  // namespace zoom_bot

#include "zoom_bot/event_journal.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace zoom_bot {
EventJournal::EventJournal(const std::filesystem::path& output, std::string session_id)
    : session_id_(std::move(session_id)) {
  if (std::filesystem::create_directories(output))
    std::filesystem::permissions(output, std::filesystem::perms::owner_all);
  fd_ = open((output / "events.jsonl").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd_ < 0) throw std::runtime_error("Cannot create a new event journal");
}

EventJournal::~EventJournal() { close(fd_); }

void EventJournal::append(const std::string& type, int64_t capture_ms, nlohmann::json payload) {
  const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  nlohmann::json event = {{"schema_version", 1}, {"session_id", session_id_},
                         {"seq", ++sequence_}, {"type", type}, {"capture_ms", capture_ms},
                         {"recorded_at_unix_ms", wall_ms}, {"data", std::move(payload)}};
  const auto line = event.dump() + "\n";
  size_t written = 0;
  while (written < line.size()) {
    const auto count = write(fd_, line.data() + written, line.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("Event journal write failed");
    written += static_cast<size_t>(count);
  }
  if (fsync(fd_) != 0) throw std::runtime_error("Event journal sync failed");
}
}  // namespace zoom_bot

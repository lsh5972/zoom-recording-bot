#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace zoom_bot {
struct JoinConfig {
  std::string session_id, meeting_id, host_user_id, passcode, display_name, sdk_jwt, user_zak;
  int64_t meeting_start_unix_ms = 0;
  int screenshot_interval_seconds = 1;
  static JoinConfig read(const std::filesystem::path& path);
};
}  // namespace zoom_bot

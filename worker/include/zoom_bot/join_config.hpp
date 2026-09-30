#pragma once

#include <filesystem>
#include <string>

namespace zoom_bot {
struct JoinConfig {
  std::string session_id, meeting_id, host_user_id, passcode, display_name, sdk_jwt, user_zak;
  static JoinConfig read(const std::filesystem::path& path);
};
}  // namespace zoom_bot

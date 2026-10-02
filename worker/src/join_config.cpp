#include "zoom_bot/join_config.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>

namespace zoom_bot {
JoinConfig JoinConfig::read(const std::filesystem::path& path) {
  try {
    std::ifstream file(path);
    const auto data = nlohmann::json::parse(file);
    if (data.at("schema_version") != 1) throw std::runtime_error("schema");
    JoinConfig config{data.at("session_id"), data.at("meeting_id"), data.at("host_user_id"),
                      data.at("passcode"), data.at("display_name"), data.at("sdk_jwt"), data.at("user_zak")};
    if (!data.at("meeting_start_unix_ms").is_number_integer()) throw std::runtime_error("start_time");
    config.meeting_start_unix_ms = data.at("meeting_start_unix_ms");
    if (data.contains("screenshot_interval_seconds")) {
      const auto& interval = data.at("screenshot_interval_seconds");
      if (!interval.is_number_integer() || interval < 1 || interval > INT32_MAX)
        throw std::runtime_error("screenshot_interval");
      config.screenshot_interval_seconds = interval;
    }
    if (!std::regex_match(config.session_id, std::regex("[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}")) ||
        !std::regex_match(config.meeting_id, std::regex("[1-9][0-9]{8,10}")) ||
        config.host_user_id.empty() || config.display_name.empty() ||
        config.sdk_jwt.empty() || config.user_zak.empty() || config.meeting_start_unix_ms <= 0)
      throw std::runtime_error("fields");
    return config;
  } catch (...) {
    // JSON parser errors can contain credential values. Never forward their text.
    throw std::runtime_error("Invalid or unreadable join configuration");
  }
}
}  // namespace zoom_bot

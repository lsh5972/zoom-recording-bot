#include "zoom_bot/capture_runtime.hpp"
#include "zoom_bot/join_config.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace zoom_bot;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Output {
  std::filesystem::path root;
  Output() { char path[] = "/tmp/zoom-bot-capture-XXXXXX"; root = mkdtemp(path); }
  ~Output() { std::filesystem::remove_all(root); }
  std::vector<nlohmann::json> events() {
    std::ifstream file(root / "events.jsonl");
    std::vector<nlohmann::json> rows;
    for (std::string line; std::getline(file, line);) rows.push_back(nlohmann::json::parse(line));
    return rows;
  }
};
int main() {
  try {
    {
      Output output;
      CaptureRuntime capture(output.root, "session");
      capture.admitted();
      capture.joined(42, "Alice");
      capture.joined(73, "Bob");
      for (int i = 0; i < 20; ++i) {
        capture.pcm({42, 16000, 1, 0, std::vector<int16_t>(160, 0)}, 9000 + i * 10);
        capture.pcm({73, 16000, 1, 0, std::vector<int16_t>(160, 0)}, 9000 + i * 10);
      }
      capture.pause("recording_permission_lost");
      for (int i = 20; i < 30; ++i) {
        capture.pcm({42, 16000, 1, 0, std::vector<int16_t>(160, 0)}, 9000 + i * 10);
        capture.pcm({73, 16000, 1, 0, std::vector<int16_t>(160, 0)}, 9000 + i * 10);
      }
      capture.close();
      require(!capture.failed(), "Capture must drain two speakers without failure");
      std::vector<int64_t> alice, bob;
      for (const auto& event : output.events()) if (event["type"] == "sdk.raw_audio.one_way") {
        (event["data"]["user_id"] == 42 ? alice : bob).push_back(event["data"]["start_ms"]);
      }
      require(alice.size() == 30 && alice == bob, "Speakers must share one SDK timestamp mapping");
      require(alice.back() - alice.front() == 290, "Source clock must remain continuous across permission pause");
      int joins = 0;
      for (const auto& event : output.events()) if (event["type"] == "participant.joined") ++joins;
      require(joins == 2, "A permission pause must preserve speaker sessions rather than fabricate rejoins");
    }
    for (int burst : {2, 3, 7}) {
      Output output;
      CaptureRuntime capture(output.root, "session");
      capture.admitted();
      for (int i = 0; i < 30; ++i) {
        const auto timestamp = 9000 + (i / burst) * burst * 10 + (i / burst) % 2;
        capture.pcm({42, 32000, 1, 0, std::vector<int16_t>(320, 0)}, timestamp);
        capture.pcm({73, 32000, 1, 0, std::vector<int16_t>(320, 0)}, timestamp);
      }
      capture.close();
      require(!capture.failed(), "Repeated 32kHz timestamps and 1ms jitter must not terminate capture");
      std::vector<int64_t> alice, bob;
      for (const auto& event : output.events()) {
        require(event["type"] != "audio.gap", "One-frame timestamp jitter must not split speech");
        if (event["type"] == "sdk.raw_audio.one_way")
          (event["data"]["user_id"] == 42 ? alice : bob).push_back(event["data"]["start_ms"]);
      }
      require(alice.size() == 30 && alice == bob, "Keep every quantized frame on the shared speaker timeline");
      for (size_t i = 1; i < alice.size(); ++i)
        require(alice[i] - alice[i - 1] == 10, "PCM sample counts must define consecutive frame times");
    }
    {
      Output output;
      CaptureRuntime capture(output.root, "session");
      capture.admitted();
      for (uint64_t timestamp : {9000, 9000, 9000, 9009, 9029, 9029})
        capture.pcm({42, 32000, 1, 0, std::vector<int16_t>(320, 0)}, timestamp);
      capture.close();
      require(!capture.failed(), "Advancing SDK timestamps after a buffered burst must not rewind PCM");
      int packets = 0;
      int64_t first = 0;
      for (const auto& event : output.events()) {
        require(event["type"] != "audio.gap", "Buffered timestamp overlaps must not split speech");
        if (event["type"] == "sdk.raw_audio.one_way") {
          const auto start = event["data"]["start_ms"].get<int64_t>();
          if (packets == 0) first = start;
          require(start == first + packets * 10, "Keep every buffered frame on the sample timeline");
          ++packets;
        }
      }
      require(packets == 6, "A buffered overlap must retain all six PCM frames");
    }
    for (bool backwards : {false, true}) {
      Output output;
      CaptureRuntime capture(output.root, "session");
      capture.admitted();
      capture.pcm({42, 32000, 1, 0, std::vector<int16_t>(320, 0)}, 9000);
      capture.pcm({42, 32000, 1, 0, std::vector<int16_t>(320, 0)}, backwards ? 8990 : 9500);
      capture.close();
      require(capture.failed() == backwards, "A backwards source clock must still fail capture");
      bool gap = false;
      for (const auto& event : output.events()) if (event["type"] == "audio.gap") gap = true;
      require(gap == !backwards, "Real timestamp gaps must remain visible rather than be smoothed away");
    }
    {
      Output output;
      CaptureRuntime capture(output.root, "session", 1);
      capture.pcm({1, 16000, 1, 0, std::vector<int16_t>(160, 0)}, 0);
      capture.close();
      require(capture.failed(), "Queue overflow must report failure, never silent success");
      bool recorded = false;
      for (const auto& event : output.events()) if (event["type"] == "capture.error") recorded = true;
      require(recorded, "Queue overflow must leave durable error evidence");
    }
    {
      Output output;
      CaptureRuntime capture(output.root, "session", 32 * 1024 * 1024, 1700000000000);
      capture.admitted();
      capture.cloud_recording(0);
      capture.cloud_recording(0);
      capture.pcm({42, 32000, 1, 0, std::vector<int16_t>(320, 0)}, 9000);
      capture.close();
      require(!capture.failed(), "A configured meeting API origin must reach the capture consumer");
      int starts = 0;
      for (const auto& event : output.events()) if (event["type"] == "cloud.recording_state") {
        ++starts;
        require(event["data"]["recording_start_unix_ms"] == 1700000000000 &&
                event["data"]["timestamp_origin"] == "meeting_api.start_time",
                "SDK status must anchor the first recording to the private join configuration");
      }
      require(starts == 1, "Duplicate starts must not reset the configured recording clock");
    }
    {
      Output output;
      const auto path = output.root / "join.json";
      std::ofstream(path) << "{\"sdk_jwt\":\"never-print-this-secret\",";
      try { JoinConfig::read(path); throw std::runtime_error("Invalid config accepted"); }
      catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "Invalid or unreadable join configuration", "Config errors must redact parser text");
      }
    }
    {
      Output output;
      const auto path = output.root / "join.json";
      nlohmann::json data = {{"schema_version", 1}, {"session_id", "00000000-0000-4000-8000-000000000001"},
                            {"meeting_id", "123456789"}, {"host_user_id", "host"}, {"passcode", "pass"},
                            {"display_name", "Recorder"}, {"sdk_jwt", "secret-jwt"}, {"user_zak", "secret-zak"},
                            {"meeting_start_unix_ms", 1700000000123}};
      std::ofstream(path) << data;
      require(JoinConfig::read(path).meeting_start_unix_ms == 1700000000123,
              "Native config must preserve the Ruby meeting start timestamp in milliseconds");
      require(JoinConfig::read(path).screenshot_interval_seconds == 1, "Old join files default to one-second screenshots");
      data["screenshot_interval_seconds"] = 5;
      std::ofstream(path) << data;
      require(JoinConfig::read(path).screenshot_interval_seconds == 5, "Preserve the configured screenshot interval");
      for (const auto& interval : nlohmann::json::array({0, -1, 1.5, "5", 2147483648LL})) {
        data["screenshot_interval_seconds"] = interval;
        std::ofstream(path) << data;
        try { JoinConfig::read(path); throw std::runtime_error("Invalid interval accepted"); }
        catch (const std::runtime_error& error) {
          require(std::string(error.what()) == "Invalid or unreadable join configuration", "Reject invalid intervals without credential leaks");
        }
      }
      data["screenshot_interval_seconds"] = 1;
      data.erase("meeting_start_unix_ms");
      std::ofstream(path) << data;
      try { JoinConfig::read(path); throw std::runtime_error("Missing origin accepted"); }
      catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "Invalid or unreadable join configuration",
                "Missing origins must be rejected without echoing credentials");
      }
    }
    std::cout << "11 capture/config cases passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

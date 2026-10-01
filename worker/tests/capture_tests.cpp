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
      const auto path = output.root / "join.json";
      std::ofstream(path) << "{\"sdk_jwt\":\"never-print-this-secret\",";
      try { JoinConfig::read(path); throw std::runtime_error("Invalid config accepted"); }
      catch (const std::runtime_error& error) {
        require(std::string(error.what()) == "Invalid or unreadable join configuration", "Config errors must redact parser text");
      }
    }
    std::cout << "8 capture/config cases passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

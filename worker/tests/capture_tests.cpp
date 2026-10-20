#include "zoom_bot/capture_runtime.hpp"
#include "zoom_bot/join_config.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sndfile.h>
#include <stdexcept>
#include <unistd.h>

using namespace zoom_bot;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Output {
  std::filesystem::path root;
  Output() { char path[] = "/tmp/zoom-bot-capture-XXXXXX"; root = mkdtemp(path); }
  ~Output() { std::filesystem::remove_all(root); }
  std::vector<nlohmann::json> chunks() {
    std::ifstream file(root / "chunks.jsonl");
    std::vector<nlohmann::json> rows;
    for (std::string line; std::getline(file, line);) {
      auto row = nlohmann::json::parse(line);
      require(row["type"] == "audio.chunk_ready", "Chunk manifest must contain only finalized WAV metadata");
      SF_INFO info{};
      auto* wav = sf_open((root / row["data"]["wav_path"].get<std::string>()).c_str(), SFM_READ, &info);
      require(wav != nullptr && info.frames > 0, "Each manifest entry must refer to an independently readable WAV");
      sf_close(wav);
      rows.push_back(std::move(row));
    }
    require(std::filesystem::file_size(root / "events.jsonl") == 0,
            "PCM, participant snapshots, screenshots and failures must not fabricate Zoom events");
    return rows;
  }
};
std::vector<int16_t> speech(int rate) {
  SF_INFO info{};
  auto* file = sf_open(FVAD_FIXTURE, SFM_READ, &info);
  require(file != nullptr && info.samplerate == 16000, "Speech fixture must load");
  std::vector<int16_t> source(info.frames);
  sf_readf_short(file, source.data(), info.frames); sf_close(file);
  if (rate == 16000) return source;
  std::vector<int16_t> doubled;
  for (auto sample : source) { doubled.push_back(sample); doubled.push_back(sample); }
  return doubled;
}
int main() {
  try {
    for (int burst : {1, 2, 3, 7}) {
      Output output;
      const auto input = speech(32000);
      CaptureRuntime capture(output.root, "session");
      capture.admitted(); capture.cloud_recording(0);
      capture.joined(42, "Alice"); capture.joined(73, "Bob");
      for (size_t i = 0; i < input.size() / 320; ++i) {
        const auto timestamp = 9000 + (i / burst) * burst * 10 + (i / burst) % 2;
        const std::vector<int16_t> frame(input.begin() + i * 320, input.begin() + (i + 1) * 320);
        if (burst == 1 && i == 200) capture.pause("recording_permission_lost");
        capture.pcm({42, 32000, 1, 0, frame}, timestamp);
        capture.pcm({73, 32000, 1, 0, frame}, timestamp);
      }
      capture.close();
      require(!capture.failed(), "Quantized timestamps and jitter must preserve both speakers across permission pauses");
      const auto chunks = output.chunks();
      std::vector<nlohmann::json> alice, bob;
      for (const auto& chunk : chunks) {
        const auto& data = chunk["data"];
        require(data["cut_reason"] != "audio_gap", "Quantized timestamps must not split speech at artificial gaps");
        require(data["participant_session_id"] == (data["user_id"] == 42 ? "user-42-join-1" : "user-73-join-1"),
                "Permission pauses must preserve the speaker's original participant session");
        (data["user_id"] == 42 ? alice : bob).push_back(data);
      }
      require(!alice.empty() && alice.size() == bob.size(), "Both speakers must produce the same voiced windows");
      for (size_t i = 0; i < alice.size(); ++i) {
        require(alice[i]["capture_start_ms"] == bob[i]["capture_start_ms"] &&
                alice[i]["capture_end_ms"] == bob[i]["capture_end_ms"],
                "Both speakers must retain the same source clock and WAV boundaries");
      }
    }
    for (bool backwards : {false, true}) {
      Output output;
      const auto input = speech(32000);
      CaptureRuntime capture(output.root, "session");
      capture.admitted(); capture.cloud_recording(0);
      for (size_t i = 0; i < input.size() / 320; ++i) {
        auto timestamp = int64_t{9000} + static_cast<int64_t>(i * 10);
        if (i >= 270) timestamp += backwards ? -20 : 500;
        capture.pcm({42, 32000, 1, 0, {input.begin() + i * 320, input.begin() + (i + 1) * 320}}, timestamp);
      }
      capture.close();
      require(capture.failed() == backwards, "A backwards source clock must still fail capture");
      const auto chunks = output.chunks();
      const bool gap = std::any_of(chunks.begin(), chunks.end(), [](const auto& row) {
        return row["data"]["cut_reason"] == "audio_gap";
      });
      require(gap == !backwards, "Real gaps must split WAVs rather than silently concatenate speech");
    }
    for (int kind : {0, 1, 2}) {
      Output output;
      CaptureRuntime capture(output.root, "session", 1024);
      if (kind == 0) capture.pcm({1, 16000, 1, 0, std::vector<int16_t>(1024, 0)}, 0);
      if (kind == 1) {
        ScreenshotFrame frame;
        frame.width = 640; frame.height = 360;
        frame.i420 = std::make_shared<const std::vector<unsigned char>>(640 * 360 * 3 / 2, 128);
        capture.screenshot(std::move(frame), 0);
      }
      if (kind == 2)
        capture.sdk_callback("IMeetingChatCtrlEvent", "onChatMsgNotification",
                             {{"nested", nlohmann::json::array({{{"name", std::string(8192, '\\')}}})}});
      capture.close();
      require(capture.failed(), "Queue limits must account for PCM, shared pixels and nested callback strings");
      require(output.chunks().empty(), "Queue overflow must not publish a WAV or fabricate a Zoom error event");
    }
    {
      Output output;
      const auto input = speech(16000);
      CaptureRuntime capture(output.root, "session", 32 * 1024 * 1024, 1700000000000);
      capture.admitted(); capture.cloud_recording(0); capture.cloud_recording(0);
      capture.pcm({42, 16000, 1, 0, input}, 9000);
      capture.close();
      require(!capture.failed(), "A configured meeting API origin must reach the capture consumer");
      const auto chunks = output.chunks();
      require(!chunks.empty(), "Configured recording must produce speech WAVs");
      for (const auto& chunk : chunks)
        require(chunk["data"]["recording_start_unix_ms"] == 1700000000000 &&
                chunk["data"]["timestamp_origin"] == "meeting_api.start_time" &&
                chunk["data"]["recording_id"] == "recording-1",
                "Duplicate starts must preserve the first recording origin in WAV metadata");
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
    std::cout << "12 capture/config cases passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

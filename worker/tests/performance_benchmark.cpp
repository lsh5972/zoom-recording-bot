#include "zoom_bot/capture_runtime.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>

using namespace zoom_bot;
namespace {
struct Output {
  std::filesystem::path root;
  Output() {
    char path[] = "/tmp/zoom-bot-performance-XXXXXX";
    auto* directory = mkdtemp(path);
    if (!directory) throw std::runtime_error("Cannot create benchmark directory");
    root = directory;
  }
  ~Output() { std::filesystem::remove_all(root); }
};
double cpu_ms() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("Cannot read CPU time");
  return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}
template <typename Work> void measure(const char* name, Work work) {
  const auto start = std::chrono::steady_clock::now();
  const auto cpu = cpu_ms();
  work();
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::cout << name << " wall_ms=" << elapsed << " cpu_ms=" << cpu_ms() - cpu << '\n';
}
}

int main() {
  try {
    Output journal_output;
    measure("journal_10000_events", [&] {
      EventJournal journal(journal_output.root, "benchmark");
      for (int i = 0; i < 10000; ++i)
        journal.append("sdk.raw_audio.one_way", i * 10,
                       {{"user_id", 42}, {"source_timestamp_ms", i * 10}, {"sample_rate", 16000},
                        {"channels", 1}, {"sample_frames", 160}});
    });
    std::ifstream journal(journal_output.root / "events.jsonl");
    int rows = 0;
    for (std::string line; std::getline(journal, line);) {
      if (nlohmann::json::parse(line).at("seq") != ++rows) throw std::runtime_error("Lost or reordered journal event");
    }
    if (rows != 10000) throw std::runtime_error("Lost journal events");

    Output capture_output;
    measure("capture_10000_pcm_10_speakers_10_images", [&] {
      CaptureRuntime capture(capture_output.root, "benchmark");
      capture.admitted(); capture.cloud_recording(0);
      for (uint32_t user = 1; user <= 10; ++user) capture.joined(user, "Speaker " + std::to_string(user));
      for (int frame = 0; frame < 1000; ++frame) {
        for (uint32_t user = 1; user <= 10; ++user)
          capture.pcm({user, 16000, 1, 0, std::vector<int16_t>(160, 0)}, frame * 10);
        if (frame % 100 == 0) {
          ScreenshotFrame image;
          image.user_id = 1; image.share_source_id = 900;
          image.width = 640; image.height = 360;
          image.i420 = std::make_shared<const std::vector<unsigned char>>(640 * 360 * 3 / 2, 128 + frame % 100);
          capture.screenshot(std::move(image), frame * 10);
        }
        capture.event("sdk.callback", {{"interface", "IMeetingAudioCtrlEvent"}, {"callback", "onUserActiveAudioChange"},
                                       {"arguments", {{"user_id", 1}, {"sequence", frame}}}});
      }
      capture.close();
      if (capture.failed()) throw std::runtime_error("Benchmark capture overflow or storage failure");
    });
    std::ifstream events(capture_output.root / "events.jsonl");
    int packets = 0, images = 0, callbacks = 0, sequence = 0;
    for (std::string line; std::getline(events, line);) {
      const auto event = nlohmann::json::parse(line);
      if (event.at("seq") != ++sequence) throw std::runtime_error("Lost or reordered capture event");
      const auto type = event.at("type").get<std::string>();
      packets += type == "sdk.raw_audio.one_way";
      images += type == "screenshot.saved";
      callbacks += type == "sdk.callback";
    }
    if (packets != 10000 || images != 10 || callbacks != 1000)
      throw std::runtime_error("Lost benchmark audio packets, images or callback events");
    std::cout << "Verified all events, 10000 PCM packets and 10 JPEGs; no Zoom SDK or emulation included\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

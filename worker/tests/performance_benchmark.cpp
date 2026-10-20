#include "zoom_bot/capture_runtime.hpp"

#include <chrono>
#include <array>
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

void moving_screenshots() {
  for (const auto& size : {std::pair{640, 360}, std::pair{1280, 720}, std::pair{1920, 1080}}) {
    for (bool limited : {true, false}) {
      std::array<ScreenshotFrame, 2> frames;
      for (int phase = 0; phase < 2; ++phase) {
        auto& frame = frames[phase];
        frame.width = size.first; frame.height = size.second; frame.limited_range = limited;
        auto pixels = std::make_shared<std::vector<unsigned char>>(frame.width * frame.height * 3 / 2);
        const auto y_size = frame.width * frame.height, uv_size = y_size / 4;
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x)
          (*pixels)[y * frame.width + x] = 16 + (x / 3 + y / 2 + phase * 47) % 220;
        for (int y = 0; y < frame.height / 2; ++y) for (int x = 0; x < frame.width / 2; ++x) {
          const auto offset = y * frame.width / 2 + x;
          (*pixels)[y_size + offset] = 96 + (x / 5 + y / 3 + phase * 13) % 64;
          (*pixels)[y_size + uv_size + offset] = 96 + (x / 7 + y / 4 + phase * 17) % 64;
        }
        frame.i420 = std::move(pixels);
      }
      Output output;
      ScreenshotWriter writer(output.root);
      RecordingTimeline recordings;
      recordings.change(0, 0, 0, 1700000000000);
      const auto label = "moving_120_jpegs_" + std::to_string(size.first) + "x" + std::to_string(size.second) +
                         (limited ? "_limited" : "_full");
      measure(label.c_str(), [&] {
        for (int i = 0; i < 120; ++i) writer.write(frames[i % 2], i * 1000, recordings);
      });
      int images = 0;
      for (const auto& file : std::filesystem::directory_iterator(output.root))
        images += file.path().extension() == ".jpg";
      if (images != 120) throw std::runtime_error("Lost moving-frame screenshots");
    }
  }
  std::cout << "Alternating textured I420 frames, 640x360 JPEG output, quality 80; includes file sync, excludes frame generation and Zoom SDK\n";
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--screenshots") { moving_screenshots(); return 0; }
    Output journal_output;
    measure("journal_10000_events", [&] {
      EventJournal journal(journal_output.root, "benchmark");
      for (int i = 0; i < 10000; ++i)
        journal.append("sdk.callback", i * 10,
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
        capture.sdk_callback("IMeetingAudioCtrlEvent", "onUserActiveAudioChange",
                             {{"user_id", 1}, {"sequence", frame}});
      }
      capture.close();
      if (capture.failed()) throw std::runtime_error("Benchmark capture overflow or storage failure");
    });
    std::ifstream events(capture_output.root / "events.jsonl");
    int images = 0, callbacks = 0, sequence = 0;
    for (std::string line; std::getline(events, line);) {
      const auto event = nlohmann::json::parse(line);
      if (event.at("seq") != ++sequence) throw std::runtime_error("Lost or reordered capture event");
      const auto type = event.at("type").get<std::string>();
      callbacks += type == "sdk.callback";
    }
    for (const auto& file : std::filesystem::directory_iterator(capture_output.root))
      images += file.path().extension() == ".jpg";
    if (images != 10 || callbacks != 1000)
      throw std::runtime_error("Lost benchmark audio packets, images or callback events");
    std::cout << "Verified 1000 Zoom callbacks and 10 JPEGs after queuing 10000 PCM packets; no Zoom SDK or emulation included\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

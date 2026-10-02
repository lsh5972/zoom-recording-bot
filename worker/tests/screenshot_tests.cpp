#include "zoom_bot/screenshot_writer.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <jpeglib.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace zoom_bot;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Output {
  std::filesystem::path root;
  Output() { char path[] = "/tmp/zoom-bot-screenshots-XXXXXX"; root = mkdtemp(path); }
  ~Output() { std::filesystem::remove_all(root); }
};
std::vector<unsigned char> decode(const std::filesystem::path& path) {
  FILE* file = fopen(path.c_str(), "rb");
  require(file != nullptr, "JPEG must exist");
  jpeg_decompress_struct decoder{}; jpeg_error_mgr error{};
  decoder.err = jpeg_std_error(&error);
  jpeg_create_decompress(&decoder); jpeg_stdio_src(&decoder, file);
  require(jpeg_read_header(&decoder, TRUE) == JPEG_HEADER_OK, "Screenshot must be an independently decodable JPEG");
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  require(decoder.output_width == 640 && decoder.output_height == 360 && decoder.output_components == 3,
          "Every screenshot must be exactly 640x360 RGB");
  std::vector<unsigned char> pixels(640 * 360 * 3);
  while (decoder.output_scanline < decoder.output_height) {
    JSAMPROW row = pixels.data() + decoder.output_scanline * 640 * 3;
    jpeg_read_scanlines(&decoder, &row, 1);
  }
  jpeg_finish_decompress(&decoder); jpeg_destroy_decompress(&decoder); fclose(file);
  return pixels;
}
ScreenshotFrame red_frame() {
  ScreenshotFrame frame;
  frame.user_id = 42; frame.share_source_id = 900; frame.width = frame.height = 4;
  frame.source_ms = 12345;
  frame.i420 = std::vector<unsigned char>(16, 82);
  frame.i420.insert(frame.i420.end(), 4, 90); frame.i420.insert(frame.i420.end(), 4, 240);
  return frame;
}
int main() {
  try {
    Output output; ScreenshotWriter writer(output.root); RecordingTimeline recordings;
    const int64_t epoch = 1700000000000;
    recordings.change(0, 0, -72000, epoch);
    auto frame = red_frame();
    const auto one = writer.write(frame, 1000, recordings);
    require(one["image_path"] == "recording-1__share__00:01:13__shot-1.jpg" &&
            one["recording_time_ms"] == 73000 && one["unix_ms"] == epoch + 1000,
            "Screenshots must use the same initial recording origin as WAVs");
    const auto path = output.root / one["image_path"].get<std::string>();
    auto pixels = decode(path);
    const auto center = (180 * 640 + 320) * 3;
    require(pixels[center] > 240 && pixels[center + 1] < 20 && pixels[center + 2] < 20,
            "I420 conversion must preserve red instead of swapping RGB channels");
    require(pixels[center - 320 * 3] < 10, "Square shares must be letterboxed rather than stretched or cropped");
    struct stat permissions{}; stat(path.c_str(), &permissions);
    require((permissions.st_mode & 0777) == 0600 && !std::filesystem::exists(path.string() + ".part"),
            "Publish private finalized images atomically without leftover partial files");
    const auto duplicate = writer.write(frame, 1000, recordings);
    require(one["image_path"] != duplicate["image_path"], "Images in the same second must never overwrite each other");
    recordings.change(3, 2000, 0, epoch);
    const auto paused = writer.write(frame, 2500, recordings);
    require(paused["recording_id"].is_null() && paused["recording_time_ms"].is_null() &&
            paused["timestamp_hms"] == "07:13:22" && paused["clock_timezone"] == "UTC+09:00" &&
            paused["image_path"].get<std::string>().find("unrecorded__share__") == 0,
            "Off-cloud screenshots must use GMT+9 wall time and preserve absolute Unix time");
    recordings.change(0, 5000, 0, epoch);
    require(writer.write(frame, 6000, recordings)["timestamp_hms"] == "00:01:15",
            "Resume must exclude the paused interval on the same recording clock");
    recordings.change(1, 7000, 0, epoch); recordings.change(0, 10000, 0, epoch);
    const auto restarted = writer.write(frame, 20000, recordings);
    require(restarted["recording_id"] == "recording-2" && restarted["timestamp_hms"] == "00:00:10",
            "A new cloud recording must restart screenshot timestamps from zero");
    for (int rotation : {0, 90, 180, 270}) {
      frame.rotation = rotation;
      auto image = writer.write(frame, 20000, recordings);
      decode(output.root / image["image_path"].get<std::string>());
    }
    frame.limited_range = false;
    std::fill(frame.i420.begin(), frame.i420.begin() + 16, 255);
    std::fill(frame.i420.begin() + 16, frame.i420.end(), 128);
    const auto white = writer.write(frame, 20000, recordings);
    pixels = decode(output.root / white["image_path"].get<std::string>());
    require(pixels[center] > 245 && pixels[center + 1] > 245 && pixels[center + 2] > 245,
            "Full-range I420 must preserve white");
    frame.i420.clear();
    try { writer.write(frame, 20000, recordings); throw std::runtime_error("Invalid frame accepted"); }
    catch (const std::runtime_error& error) {
      require(std::string(error.what()) == "Invalid shared-screen frame", "Reject truncated I420 without reading out of bounds");
    }
    for (const auto& file : std::filesystem::directory_iterator(output.root))
      require(file.path().extension() == ".jpg", "All screenshots must stay in the flat meeting output folder");
    std::cout << "Screenshot encoding, geometry, permissions and recording clocks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

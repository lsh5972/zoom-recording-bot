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
  auto pixels = std::make_shared<std::vector<unsigned char>>(16, 82);
  pixels->insert(pixels->end(), 4, 90); pixels->insert(pixels->end(), 4, 240);
  frame.i420 = std::move(pixels);
  return frame;
}

void planar_colors_and_padding() {
  Output output; ScreenshotWriter writer(output.root); RecordingTimeline recordings;
  recordings.change(0, 0, 0, 1700000000000);
  const int expected[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
  const int positions[][4] = {{0, 1, 2, 3}, {2, 0, 3, 1}, {3, 2, 1, 0}, {1, 3, 0, 2}};
  for (bool limited : {true, false}) for (const auto& size : {std::pair{640, 360}, std::pair{641, 359}}) {
    ScreenshotFrame frame;
    frame.width = size.first; frame.height = size.second; frame.limited_range = limited;
    const auto y_size = frame.width * frame.height;
    const auto uv_width = (frame.width + 1) / 2, uv_height = (frame.height + 1) / 2;
    const auto uv_size = uv_width * uv_height;
    const int limited_yuv[][3] = {{81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {235, 128, 128}};
    const int full_yuv[][3] = {{76, 85, 255}, {150, 44, 21}, {29, 255, 107}, {255, 128, 128}};
    const auto* colors = limited ? limited_yuv : full_yuv;
    auto planes = std::make_shared<std::vector<unsigned char>>(y_size + 2 * uv_size);
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x)
      (*planes)[y * frame.width + x] = colors[(y >= frame.height / 2) * 2 + (x >= frame.width / 2)][0];
    for (int y = 0; y < uv_height; ++y) for (int x = 0; x < uv_width; ++x) {
      const auto* color = colors[(y >= uv_height / 2) * 2 + (x >= uv_width / 2)];
      (*planes)[y_size + y * uv_width + x] = color[1];
      (*planes)[y_size + uv_size + y * uv_width + x] = color[2];
    }
    frame.i420 = std::move(planes);
    for (int turn = 0; turn < 4; ++turn) {
      frame.rotation = turn * 90;
      const auto image = writer.write(frame, 0, recordings);
      const auto pixels = decode(output.root / image["image_path"].get<std::string>());
      const int rotated_width = turn % 2 ? frame.height : frame.width;
      const int rotated_height = turn % 2 ? frame.width : frame.height;
      const auto scale = std::min(640.0 / rotated_width, 360.0 / rotated_height);
      const int w = rotated_width * scale, h = rotated_height * scale;
      for (int quadrant = 0; quadrant < 4; ++quadrant) {
        const int x = (640 - w) / 2 + w * (quadrant % 2 ? 3 : 1) / 4;
        const int y = (360 - h) / 2 + h * (quadrant / 2 ? 3 : 1) / 4;
        for (int channel = 0; channel < 3; ++channel)
          require(std::abs(static_cast<int>(pixels[(y * 640 + x) * 3 + channel]) -
                           expected[positions[turn][quadrant]][channel]) < 15,
                  "Planar JPEG must preserve chroma, color range and rotation for even and odd source dimensions");
      }
      const auto bottom = (359 * 640 + 639) * 3;
      if (turn == 0 && frame.width == 640)
        require(pixels[bottom] > 245 && pixels[bottom + 1] > 245 && pixels[bottom + 2] > 245,
                "The final JPEG MCU must replicate the last valid plane rows without corrupting the image bottom");
      if (turn % 2)
        require(pixels[0] < 5 && pixels[1] < 5 && pixels[2] < 5,
                "Planar letterboxing must remain neutral black after rotation");
    }
  }
}

int main() {
  try {
    planar_colors_and_padding();
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
    require(decode(output.root / duplicate["image_path"].get<std::string>()) == pixels,
            "Reused JPEG data must still publish a complete independently decodable file");
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
    auto white_pixels = std::make_shared<std::vector<unsigned char>>(24, 128);
    std::fill(white_pixels->begin(), white_pixels->begin() + 16, 255);
    frame.i420 = white_pixels;
    const auto white = writer.write(frame, 20000, recordings);
    pixels = decode(output.root / white["image_path"].get<std::string>());
    require(pixels[center] > 245 && pixels[center + 1] > 245 && pixels[center + 2] > 245,
            "Full-range I420 must preserve white");
    frame.width = 8; frame.height = 2; frame.rotation = 0;
    const auto landscape = writer.write(frame, 21000, recordings);
    pixels = decode(output.root / landscape["image_path"].get<std::string>());
    require(pixels[(20 * 640 + 320) * 3] < 10 && pixels[center] > 245,
            "Changing geometry with the same pixel buffer must invalidate cached letterboxing");
    frame.rotation = 90;
    const auto portrait = writer.write(frame, 22000, recordings);
    pixels = decode(output.root / portrait["image_path"].get<std::string>());
    require(pixels[(180 * 640 + 200) * 3] < 10 && pixels[center] > 245,
            "Changing rotation with the same pixel buffer must invalidate cached geometry");
    frame.width = frame.height = 4; frame.rotation = 0; frame.limited_range = true;
    auto dark_pixels = std::make_shared<std::vector<unsigned char>>(24, 128);
    std::fill(dark_pixels->begin(), dark_pixels->begin() + 16, 16);
    frame.i420 = dark_pixels;
    const auto limited = writer.write(frame, 23000, recordings);
    require(decode(output.root / limited["image_path"].get<std::string>())[center] < 3,
            "Limited-range luminance 16 must produce black");
    frame.limited_range = false;
    const auto full = writer.write(frame, 24000, recordings);
    require(decode(output.root / full["image_path"].get<std::string>())[center] >= 14,
            "Changing color range with the same pixel buffer must invalidate cached colors");
    auto quadrants = std::make_shared<std::vector<unsigned char>>(24, 128);
    const int levels[] = {16, 82, 145, 235};
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column)
      (*quadrants)[row * 4 + column] = levels[(row / 2) * 2 + column / 2];
    frame.i420 = quadrants; frame.limited_range = true;
    const int expected[][4] = {{0, 77, 150, 255}, {150, 0, 255, 77},
                              {255, 150, 77, 0}, {77, 255, 0, 150}};
    for (int turn = 0; turn < 4; ++turn) {
      frame.rotation = turn * 90;
      const auto image = writer.write(frame, 25000 + turn, recordings);
      pixels = decode(output.root / image["image_path"].get<std::string>());
      for (int corner = 0; corner < 4; ++corner) {
        const int x = corner % 2 == 0 ? 230 : 410, y = corner / 2 == 0 ? 90 : 270;
        require(std::abs(static_cast<int>(pixels[(y * 640 + x) * 3]) - expected[turn][corner]) < 8,
                "Coordinate lookup tables must preserve each image quadrant in all four rotations");
      }
    }
    frame.i420.reset();
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

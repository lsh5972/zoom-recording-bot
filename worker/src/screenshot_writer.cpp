#include "zoom_bot/screenshot_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <jpeglib.h>
#include <setjmp.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace zoom_bot {
namespace {
constexpr int width = 640, height = 360;
struct JpegError { jpeg_error_mgr manager; jmp_buf target; };
void jpeg_error(j_common_ptr jpeg) { longjmp(reinterpret_cast<JpegError*>(jpeg->err)->target, 1); }
struct JpegEncoder {
  jpeg_compress_struct compressor{};
  JpegError error{};
  unsigned char* bytes = nullptr;
  unsigned long size = 0;
  ~JpegEncoder() { jpeg_destroy_compress(&compressor); std::free(bytes); }
};

std::vector<unsigned char> rgb(const ScreenshotFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192)
    throw std::runtime_error("Invalid shared-screen size");
  const auto pixels = static_cast<size_t>(frame.width) * frame.height;
  const auto chroma_width = (frame.width + 1) / 2, chroma_height = (frame.height + 1) / 2;
  if (pixels > 32 * 1024 * 1024 ||
      !frame.i420 || frame.i420->size() != pixels + 2 * static_cast<size_t>(chroma_width) * chroma_height ||
      (frame.rotation != 0 && frame.rotation != 90 && frame.rotation != 180 && frame.rotation != 270))
    throw std::runtime_error("Invalid shared-screen frame");
  const bool swapped = frame.rotation == 90 || frame.rotation == 270;
  const int rotated_width = swapped ? frame.height : frame.width;
  const int rotated_height = swapped ? frame.width : frame.height;
  const double scale = std::min(static_cast<double>(width) / rotated_width,
                                static_cast<double>(height) / rotated_height);
  const int image_width = std::max(1, static_cast<int>(rotated_width * scale));
  const int image_height = std::max(1, static_cast<int>(rotated_height * scale));
  const int left = (width - image_width) / 2, top = (height - image_height) / 2;
  const auto* y = frame.i420->data();
  const auto* u = y + pixels;
  const auto* v = u + static_cast<size_t>(chroma_width) * chroma_height;
  std::vector<unsigned char> result(width * height * 3, 0);
  for (int row = 0; row < image_height; ++row) for (int column = 0; column < image_width; ++column) {
    const int rx = column * rotated_width / image_width, ry = row * rotated_height / image_height;
    const int sx = frame.rotation == 90 ? ry : frame.rotation == 180 ? frame.width - rx - 1 :
                   frame.rotation == 270 ? frame.width - ry - 1 : rx;
    const int sy = frame.rotation == 90 ? frame.height - rx - 1 : frame.rotation == 180 ? frame.height - ry - 1 :
                   frame.rotation == 270 ? rx : ry;
    const int uv = (sy / 2) * chroma_width + sx / 2;
    const int luminance = y[sy * frame.width + sx];
    const int cb = u[uv] - 128, cr = v[uv] - 128;
    const int base = frame.limited_range ? 298 * (luminance - 16) : 256 * luminance;
    const int red = base + (frame.limited_range ? 409 : 359) * cr;
    const int green = base - (frame.limited_range ? 100 : 88) * cb - (frame.limited_range ? 208 : 183) * cr;
    const int blue = base + (frame.limited_range ? 516 : 454) * cb;
    auto* pixel = result.data() + ((row + top) * width + column + left) * 3;
    pixel[0] = std::clamp((red + 128) / 256, 0, 255);
    pixel[1] = std::clamp((green + 128) / 256, 0, 255);
    pixel[2] = std::clamp((blue + 128) / 256, 0, 255);
  }
  return result;
}

std::vector<unsigned char> encode(const ScreenshotFrame& frame) {
  const auto pixels = rgb(frame);
  // Heap state remains valid after libjpeg's longjmp; RAII releases its destination buffer.
  auto encoder = std::make_unique<JpegEncoder>();
  auto& compressor = encoder->compressor;
  auto& error = encoder->error;
  compressor.err = jpeg_std_error(&error.manager);
  error.manager.error_exit = jpeg_error;
  if (setjmp(error.target)) throw std::runtime_error("JPEG encoding failed");
  jpeg_create_compress(&compressor);
  jpeg_mem_dest(&compressor, &encoder->bytes, &encoder->size);
  compressor.image_width = width;
  compressor.image_height = height;
  compressor.input_components = 3;
  compressor.in_color_space = JCS_RGB;
  jpeg_set_defaults(&compressor);
  jpeg_set_quality(&compressor, 80, TRUE);
  jpeg_start_compress(&compressor, TRUE);
  while (compressor.next_scanline < compressor.image_height) {
    JSAMPROW row = const_cast<unsigned char*>(pixels.data() + compressor.next_scanline * width * 3);
    jpeg_write_scanlines(&compressor, &row, 1);
  }
  jpeg_finish_compress(&compressor);
  return std::vector<unsigned char>(encoder->bytes, encoder->bytes + encoder->size);
}

void publish(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
  const auto temporary = path.string() + ".part";
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) throw std::runtime_error("Cannot create screenshot");
  size_t used = 0;
  while (used < bytes.size()) {
    const auto count = write(fd, bytes.data() + used, bytes.size() - used);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { close(fd); throw std::runtime_error("Screenshot write failed"); }
    used += static_cast<size_t>(count);
  }
  const bool synced = fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!synced || !closed) throw std::runtime_error("Screenshot write failed");
  if (link(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Screenshot publish failed");
  unlink(temporary.c_str());
  int directory = open(path.parent_path().c_str(), O_RDONLY);
  if (directory < 0) throw std::runtime_error("Cannot sync screenshot directory");
  const bool published = fsync(directory) == 0;
  close(directory);
  if (!published) throw std::runtime_error("Screenshot directory sync failed");
}
}

nlohmann::json ScreenshotWriter::write(const ScreenshotFrame& frame, int64_t capture_ms,
                                     const RecordingTimeline& recordings) {
  const auto* span = recordings.at(capture_ms);
  if (!span) throw std::runtime_error("Screenshot recording clock unavailable");
  const bool recorded = span->sequence > 0;
  const auto unix_ms = recordings.wall_ms(capture_ms);
  const auto timestamp_ms = recorded ? span->offset_ms + capture_ms - span->start_ms : unix_ms;
  auto seconds = recorded ? timestamp_ms / 1000 : (timestamp_ms / 1000 + 9 * 3600) % 86400;
  std::ostringstream clock;
  clock << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
        << std::setw(2) << seconds / 60 % 60 << ':' << std::setw(2) << seconds % 60;
  const auto filename = span->id() + "__share__" + clock.str() + "__shot-" + std::to_string(++sequence_) + ".jpg";
  if (encoded_jpeg_.empty() || frame.i420 != encoded_frame_.i420 || frame.width != encoded_frame_.width ||
      frame.height != encoded_frame_.height || frame.rotation != encoded_frame_.rotation ||
      frame.limited_range != encoded_frame_.limited_range) {
    encoded_jpeg_ = encode(frame);
    encoded_frame_ = frame;
  }
  publish(output_ / filename, encoded_jpeg_);
  return {{"image_path", filename}, {"width", width}, {"height", height},
          {"user_id", frame.user_id}, {"share_source_id", frame.share_source_id},
          {"source_timestamp_ms", frame.source_ms}, {"capture_ms", capture_ms}, {"unix_ms", unix_ms},
          {"recording_id", recorded ? nlohmann::json(span->id()) : nlohmann::json(nullptr)},
          {"recording_time_ms", recorded ? nlohmann::json(timestamp_ms) : nlohmann::json(nullptr)},
          {"timestamp_hms", clock.str()}, {"timestamp_origin", span->origin_source},
          {"clock_timezone", recorded ? nlohmann::json(nullptr) : nlohmann::json("UTC+09:00")}};
}
}  // namespace zoom_bot

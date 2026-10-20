#include "zoom_bot/screenshot_writer.hpp"

#include <algorithm>
#include <array>
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

std::vector<unsigned char> ycbcr(const ScreenshotFrame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 8192 || frame.height > 8192)
    throw std::runtime_error("Invalid shared-screen size");
  const auto pixels = static_cast<size_t>(frame.width) * frame.height;
  const auto chroma_width = (frame.width + 1) / 2, chroma_height = (frame.height + 1) / 2;
  if (pixels > 32 * 1024 * 1024 ||
      !frame.i420 || frame.i420->size() != pixels + 2 * static_cast<size_t>(chroma_width) * chroma_height ||
      (frame.rotation != 0 && frame.rotation != 90 && frame.rotation != 180 && frame.rotation != 270))
    throw std::runtime_error("Invalid shared-screen frame");
  // JPEG uses full-range YCbCr. Keep full-range, unscaled I420 in its original planes.
  if (frame.width == width && frame.height == height && frame.rotation == 0 && !frame.limited_range) return {};
  std::array<unsigned char, 256> luma, chroma;
  for (int value = 0; value < 256; ++value) {
    luma[value] = frame.limited_range ? std::clamp(((value - 16) * 255 + 109) / 219, 0, 255) : value;
    chroma[value] = frame.limited_range ?
        std::clamp(128 + ((value - 128) * 255 + (value >= 128 ? 112 : -112)) / 224, 0, 255) : value;
  }
  std::vector<unsigned char> result(width * height * 3 / 2, 128);
  auto* out_y = result.data();
  auto* out_u = out_y + width * height;
  auto* out_v = out_u + width * height / 4;
  const auto* y = frame.i420->data();
  const auto* u = y + pixels;
  const auto* v = u + static_cast<size_t>(chroma_width) * chroma_height;
  if (frame.width == width && frame.height == height && frame.rotation == 0) {
    for (int i = 0; i < width * height; ++i) out_y[i] = luma[y[i]];
    for (int i = 0; i < width * height / 4; ++i) { out_u[i] = chroma[u[i]]; out_v[i] = chroma[v[i]]; }
    return result;
  }
  const bool swapped = frame.rotation == 90 || frame.rotation == 270;
  const int rotated_width = swapped ? frame.height : frame.width;
  const int rotated_height = swapped ? frame.width : frame.height;
  const double scale = std::min(static_cast<double>(width) / rotated_width,
                                static_cast<double>(height) / rotated_height);
  const int image_width = std::max(1, static_cast<int>(rotated_width * scale));
  const int image_height = std::max(1, static_cast<int>(rotated_height * scale));
  const int left = (width - image_width) / 2, top = (height - image_height) / 2;
  std::fill(out_y, out_u, 0);  // Black letterbox in full-range YCbCr is (0, 128, 128).
  std::array<int, width> columns;
  std::array<int, height> rows;
  for (int column = 0; column < image_width; ++column) columns[column] = column * rotated_width / image_width;
  for (int row = 0; row < image_height; ++row) rows[row] = row * rotated_height / image_height;
  auto source = [&](int column, int row) {
    const int rx = columns[column], ry = rows[row];
    const int sx = frame.rotation == 90 ? ry : frame.rotation == 180 ? frame.width - rx - 1 :
                   frame.rotation == 270 ? frame.width - ry - 1 : rx;
    const int sy = frame.rotation == 90 ? frame.height - rx - 1 : frame.rotation == 180 ? frame.height - ry - 1 :
                   frame.rotation == 270 ? rx : ry;
    return std::pair{sx, sy};
  };
  for (int row = 0; row < image_height; ++row) for (int column = 0; column < image_width; ++column) {
    const auto [sx, sy] = source(column, row);
    out_y[(row + top) * width + column + left] = luma[y[sy * frame.width + sx]];
  }
  for (int row = 0; row < height / 2; ++row) for (int column = 0; column < width / 2; ++column) {
    const int x = column * 2 - left, y_position = row * 2 - top;
    if (x < 0 || y_position < 0 || x >= image_width || y_position >= image_height) continue;
    const auto [sx, sy] = source(x, y_position);
    const auto uv = (sy / 2) * chroma_width + sx / 2;
    out_u[row * width / 2 + column] = chroma[u[uv]];
    out_v[row * width / 2 + column] = chroma[v[uv]];
  }
  return result;
}

std::vector<unsigned char> encode(const ScreenshotFrame& frame) {
  const auto pixels = ycbcr(frame);
  const auto* y = pixels.empty() ? frame.i420->data() : pixels.data();
  const auto* u = y + width * height;
  const auto* v = u + width * height / 4;
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
  compressor.in_color_space = JCS_YCbCr;
  jpeg_set_defaults(&compressor);
  jpeg_set_colorspace(&compressor, JCS_YCbCr);
  compressor.raw_data_in = TRUE;
  compressor.comp_info[0].h_samp_factor = compressor.comp_info[0].v_samp_factor = 2;
  for (int plane = 1; plane < 3; ++plane)
    compressor.comp_info[plane].h_samp_factor = compressor.comp_info[plane].v_samp_factor = 1;
  jpeg_set_quality(&compressor, 80, TRUE);
  jpeg_start_compress(&compressor, TRUE);
  while (compressor.next_scanline < compressor.image_height) {
    JSAMPROW y_rows[16], u_rows[8], v_rows[8];
    for (int row = 0; row < 16; ++row)
      y_rows[row] = const_cast<unsigned char*>(y + std::min<int>(compressor.next_scanline + row, height - 1) * width);
    for (int row = 0; row < 8; ++row) {
      const auto offset = std::min<int>(compressor.next_scanline / 2 + row, height / 2 - 1) * width / 2;
      u_rows[row] = const_cast<unsigned char*>(u + offset);
      v_rows[row] = const_cast<unsigned char*>(v + offset);
    }
    JSAMPARRAY planes[] = {y_rows, u_rows, v_rows};
    jpeg_write_raw_data(&compressor, planes, 16);
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

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zoom_bot {
class WavWriter {
 public:
  explicit WavWriter(std::filesystem::path output);
  // The returned relative path becomes visible only after the WAV is finalized.
  std::string write(const std::string& relative_path, int rate, int channels,
                    const std::vector<int16_t>& samples);

 private:
  std::filesystem::path output_;
};
}  // namespace zoom_bot

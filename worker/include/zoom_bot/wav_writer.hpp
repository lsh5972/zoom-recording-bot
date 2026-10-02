#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zoom_bot {
class WavWriter {
 public:
  explicit WavWriter(std::filesystem::path output);
  static std::string speaker_label(const std::string& display_name);
  uint64_t next_sequence() { return ++sequence_; }
  // The returned relative path becomes visible only after the WAV is finalized.
  std::string write(const std::string& relative_path, int rate, int channels,
                    const std::vector<int16_t>& samples);

 private:
  std::filesystem::path output_;
  uint64_t sequence_ = 0;
};
}  // namespace zoom_bot

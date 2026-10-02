#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

struct Fvad;
namespace zoom_bot {
class SpeechDetector {
 public:
  explicit SpeechDetector(int rate);
  ~SpeechDetector();
  SpeechDetector(const SpeechDetector&) = delete;
  SpeechDetector& operator=(const SpeechDetector&) = delete;
  bool voiced(const std::vector<int16_t>& interleaved, int channels);
  bool voiced(const int16_t* interleaved, size_t samples, int channels);
  void reset();

 private:
  Fvad* vad_;
  int rate_;
  std::vector<int16_t> mono_;
};
}  // namespace zoom_bot

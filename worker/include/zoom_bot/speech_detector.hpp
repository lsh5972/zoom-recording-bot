#pragma once

#include <cstdint>
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
  void reset();

 private:
  Fvad* vad_;
  int rate_;
};
}  // namespace zoom_bot

#include "zoom_bot/speech_detector.hpp"

#include <fvad.h>
#include <stdexcept>

namespace zoom_bot {
SpeechDetector::SpeechDetector(int rate) : vad_(fvad_new()), rate_(rate) {
  if (!vad_) throw std::runtime_error("Cannot allocate VAD");
  if (fvad_set_sample_rate(vad_, rate_) != 0) {
    fvad_free(vad_);
    throw std::invalid_argument("VAD requires 8000, 16000, 32000 or 48000 Hz");
  }
  fvad_set_mode(vad_, 2);
}
SpeechDetector::~SpeechDetector() { fvad_free(vad_); }

void SpeechDetector::reset() {
  fvad_reset(vad_);
  fvad_set_sample_rate(vad_, rate_);
  fvad_set_mode(vad_, 2);
}

bool SpeechDetector::voiced(const std::vector<int16_t>& interleaved, int channels) {
  return voiced(interleaved.data(), interleaved.size(), channels);
}

bool SpeechDetector::voiced(const int16_t* interleaved, size_t samples, int channels) {
  if (!interleaved || channels < 1 || channels > 2 || samples != static_cast<size_t>(rate_ / 100 * channels))
    throw std::invalid_argument("VAD input must be a 10 ms mono or stereo frame");
  if (channels == 2) {
    mono_.resize(samples / channels);
    for (size_t i = 0; i < mono_.size(); ++i)
      mono_[i] = static_cast<int16_t>((static_cast<int>(interleaved[i * 2]) + interleaved[i * 2 + 1]) / 2);
    interleaved = mono_.data();
  }
  const int result = fvad_process(vad_, interleaved, samples / channels);
  if (result < 0) throw std::runtime_error("VAD rejected frame");
  return result == 1;
}
}  // namespace zoom_bot

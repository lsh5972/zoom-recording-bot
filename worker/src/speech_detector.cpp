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
  if (channels < 1 || channels > 2 || interleaved.size() != static_cast<size_t>(rate_ / 100 * channels))
    throw std::invalid_argument("VAD input must be a 10 ms mono or stereo frame");
  std::vector<int16_t> mono(interleaved.size() / channels);
  for (size_t i = 0; i < mono.size(); ++i) {
    int sum = 0;
    for (int channel = 0; channel < channels; ++channel) sum += interleaved[i * channels + channel];
    mono[i] = static_cast<int16_t>(sum / channels);
  }
  const int result = fvad_process(vad_, mono.data(), mono.size());
  if (result < 0) throw std::runtime_error("VAD rejected frame");
  return result == 1;
}
}  // namespace zoom_bot

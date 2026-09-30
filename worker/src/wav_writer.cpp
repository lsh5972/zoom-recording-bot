#include "zoom_bot/wav_writer.hpp"

#include <fcntl.h>
#include <regex>
#include <sndfile.h>
#include <stdexcept>
#include <unistd.h>

namespace zoom_bot {
WavWriter::WavWriter(std::filesystem::path output) : output_(std::move(output)) {
  std::filesystem::create_directories(output_);
}

std::string WavWriter::write(const std::string& relative_path, int rate, int channels,
                           const std::vector<int16_t>& samples) {
  const std::regex filename("[a-zA-Z0-9_-]+__[0-9]{2,}:[0-9]{2}:[0-9]{2}-[0-9]{2,}:[0-9]{2}:[0-9]{2}__chunk-[0-9]+\\.wav");
  if (!std::regex_match(relative_path, filename) || samples.empty() ||
      channels <= 0 || samples.size() % channels != 0)
    throw std::invalid_argument("Invalid WAV chunk");
  const auto final = output_ / relative_path;
  const auto temporary = final.string() + ".part";
  int fd = open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) throw std::runtime_error("Cannot create WAV chunk");
  SF_INFO info{};
  info.samplerate = rate;
  info.channels = channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  SNDFILE* file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE);
  if (!file) {
    close(fd);
    throw std::runtime_error("Cannot open WAV chunk");
  }
  const auto frames = static_cast<sf_count_t>(samples.size() / channels);
  const bool complete = sf_writef_short(file, samples.data(), frames) == frames;
  const bool finalized = sf_close(file) == 0;
  const bool synced = fsync(fd) == 0;
  close(fd);
  if (!complete || !finalized || !synced) throw std::runtime_error("WAV chunk write failed");
  // link() publishes without overwriting any existing finalized chunk.
  if (link(temporary.c_str(), final.c_str()) != 0) throw std::runtime_error("WAV publish failed");
  unlink(temporary.c_str());
  int directory = open(final.parent_path().c_str(), O_RDONLY);
  if (directory < 0) throw std::runtime_error("Cannot sync WAV directory");
  const bool published = fsync(directory) == 0;
  close(directory);
  if (!published) throw std::runtime_error("WAV directory sync failed");
  return relative_path;
}
}  // namespace zoom_bot

#include "zoom_bot/audio_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <regex>
#include <sndfile.h>
#include <thread>

using namespace zoom_bot;
struct Track {
  uint32_t user_id;
  SF_INFO info{};
  std::unique_ptr<SNDFILE, decltype(&sf_close)> file{nullptr, sf_close};
  int64_t start_ms = 0;
  bool done = false;
};

int main(int argc, char** argv) {
  try {
    bool realtime = argc > 1 && std::string(argv[1]) == "--realtime";
    int first = realtime ? 2 : 1;
    if (argc - first < 4 || (argc - first - 2) % 2 != 0)
      throw std::invalid_argument("Usage: zoom-bot-replay [--realtime] SESSION_ID OUTPUT USER_ID INPUT.wav [USER_ID INPUT.wav ...]");
    const std::string session_id = argv[first];
    if (!std::regex_match(session_id, std::regex("[a-zA-Z0-9_-]+")))
      throw std::invalid_argument("Invalid replay session ID");
    std::vector<Track> tracks;
    for (int i = first + 2; i < argc; i += 2) {
      const std::string user_id = argv[i];
      if (!std::regex_match(user_id, std::regex("[0-9]+")) || std::stoull(user_id) > UINT32_MAX)
        throw std::invalid_argument("Invalid replay user ID");
      Track track;
      track.user_id = static_cast<uint32_t>(std::stoull(user_id));
      track.file.reset(sf_open(argv[i + 1], SFM_READ, &track.info));
      if (!track.file) throw std::runtime_error("Cannot open replay WAV");
      if (std::any_of(tracks.begin(), tracks.end(), [&](const auto& other) { return other.user_id == track.user_id; }))
        throw std::invalid_argument("Duplicate replay user ID");
      tracks.push_back(std::move(track));
    }
    EventJournal events(argv[first + 1], session_id);
    WavWriter wav(argv[first + 1]);
    AudioPipeline pipeline(events, wav);
    events.append("capture.started", 0, {{"source", "wav_replay"}, {"realtime", realtime}});
    for (const auto& track : tracks) pipeline.joined(track.user_id, "Replay", 0);
    const auto started = std::chrono::steady_clock::now();
    int64_t end_ms = 0;
    while (true) {
      auto found = std::min_element(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) {
        if (a.done != b.done) return !a.done;
        return a.start_ms < b.start_ms;
      });
      if (found == tracks.end() || found->done) break;
      auto& track = *found;
      std::vector<int16_t> samples(static_cast<size_t>(track.info.samplerate / 100 * track.info.channels));
      auto frames = sf_readf_short(track.file.get(), samples.data(), track.info.samplerate / 100);
      if (frames == 0) {
        pipeline.left(track.user_id, track.start_ms);
        track.done = true;
        continue;
      }
      samples.resize(static_cast<size_t>(frames * track.info.channels));
      if (realtime) std::this_thread::sleep_until(started + std::chrono::milliseconds(track.start_ms));
      pipeline.consume({track.user_id, track.info.samplerate, track.info.channels, track.start_ms, std::move(samples)});
      track.start_ms += (frames * 1000 + track.info.samplerate - 1) / track.info.samplerate;
      end_ms = std::max(end_ms, track.start_ms);
    }
    pipeline.finish("replay_complete", end_ms);
    std::cout << "Replay complete. No Zoom meeting was joined. Output: " << argv[first + 1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

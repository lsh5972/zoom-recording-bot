#include "zoom_bot/audio_pipeline.hpp"

#include <fstream>
#include <functional>
#include <iostream>
#include <sndfile.h>
#include <stdexcept>
#include <unistd.h>

using namespace zoom_bot;
using Json = nlohmann::json;

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct Output {
  std::filesystem::path root;
  RecordingTimeline recordings;
  Output() {
    recordings.change(0, 0, 0, 0);
    char path[] = "/tmp/zoom-bot-audio-test-XXXXXX";
    auto directory = mkdtemp(path);
    if (!directory) throw std::runtime_error("Cannot create test directory");
    root = directory;
  }
  ~Output() { std::filesystem::remove_all(root); }
  std::vector<Json> events(const std::string& type = "") const {
    std::vector<Json> result;
    std::ifstream file(root / "events.jsonl");
    for (std::string line; std::getline(file, line);) {
      auto event = Json::parse(line);
      if (type.empty() || event["type"] == type) result.push_back(std::move(event));
    }
    return result;
  }
};

std::vector<int16_t> read_wav(const std::filesystem::path& path, SF_INFO& info) {
  SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
  check(file != nullptr, "Finalized WAV must be independently readable");
  std::vector<int16_t> samples(static_cast<size_t>(info.frames * info.channels));
  check(sf_readf_short(file, samples.data(), info.frames) == info.frames, "WAV must contain all declared frames");
  check(sf_close(file) == 0, "WAV close must succeed");
  return samples;
}

void feed(AudioSegmenter& segmenter, int start, int frames, bool voiced, int16_t sample = 1234) {
  for (int i = 0; i < frames; ++i) segmenter.consume({start + i * 10, std::vector<int16_t>(160, sample)}, voiced);
}

void silence_boundary_and_pre_roll() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 20, false, 0);
  feed(segmenter, 200, 30, true);
  check(output.events("audio.chunk_ready").empty(), "Short speech must remain open before closure");
  feed(segmenter, 500, 40, false, 0);
  segmenter.finish("stop");
  auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 1, "Silence must finalize exactly one chunk");
  const auto& metadata = ready[0]["data"];
  check(metadata["start_ms"] == 0 && metadata["end_ms"] == 900, "Chunk must include pre-roll and silence tail");
  check(metadata["cut_reason"] == "silence", "Silence boundary reason must survive");
  check(metadata["wav_path"] == "recording-1__Alice__00:00:00-00:00:00__chunk-1.wav",
        "Filename must include the speaker name and cloud recording time range");
  SF_INFO info{};
  auto samples = read_wav(output.root / metadata["wav_path"].get<std::string>(), info);
  check(info.frames == 14400 && info.samplerate == 16000 && info.channels == 1, "WAV format/duration mismatch");
  check(samples[3199] == 0 && samples[3200] == 1234 && samples[7999] == 1234 && samples[8000] == 0,
        "WAV samples must preserve pre-roll, voice and tail positions");
  check(output.events("speech_on").size() == 1 && output.events("speech_off").size() == 1,
        "Utterance must have one pair of speech events");
  check(output.events("speech_on")[0]["capture_ms"] == 200, "Speech onset must exclude pre-roll");
  check(output.events("speech_off")[0]["data"]["speech_end_ms"] == 500, "Speech offset must exclude silence tail");
  auto events = output.events();
  for (size_t i = 0; i < events.size(); ++i) check(events[i]["seq"] == i + 1, "Event sequence must be contiguous");
}

void long_speech_publishes_during_utterance() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 2999, true);
  check(output.events("audio.chunk_ready").empty(), "Speech under 30 s must remain a single open chunk");
  feed(segmenter, 29990, 1, true);
  check(output.events("audio.chunk_ready").size() == 1, "Long speech must publish at 30 s before speech_off");
  check(output.events("speech_off").empty(), "Chunk cut must not end speech");
  feed(segmenter, 30000, 3500, true);
  segmenter.finish("stop");
  auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 3, "Long speech must produce consecutive windows");
  int starts[] = {0, 29800, 59800};
  int ends[] = {30000, 60000, 65000};
  for (size_t i = 0; i < ready.size(); ++i) {
    auto data = ready[i]["data"];
    check(data["start_ms"] == starts[i] && data["end_ms"] == ends[i], "Forced windows must preserve overlap");
    check(data["overlap_ms"] == (i == 0 ? 0 : 200), "Overlap metadata mismatch");
    check(data["utterance_id"] == ready[0]["data"]["utterance_id"], "Forced cuts must retain utterance ID");
    SF_INFO info{};
    read_wav(output.root / data["wav_path"].get<std::string>(), info);
    check(info.frames == (ends[i] - starts[i]) * 16, "Independent WAV duration mismatch");
  }
  check(output.events("speech_on").size() == 1 && output.events("speech_off").size() == 1,
        "Long speech must remain a single utterance");
}

void exact_forced_boundary_avoids_duplicate_only_chunk() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 3000, true);
  segmenter.finish("permission_revoked");
  check(output.events("audio.chunk_ready").size() == 1, "Stop at forced boundary must not republish overlap alone");
  check(output.events("speech_off")[0]["data"]["reason"] == "permission_revoked", "Stop reason must be recorded");
}

void thirty_seconds_counts_speech_without_pre_roll_or_silence_tail() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 20, false, 0);
  feed(segmenter, 200, 2999, true);
  check(output.events("audio.chunk_ready").empty(), "Pre-roll must not count toward the 30 s speech limit");
  feed(segmenter, 30190, 40, false, 0);
  auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 1 && ready[0]["data"]["cut_reason"] == "silence",
        "Speech ending before 30 s must not be force-cut during the silence tail");
}

void final_partial_frame_preserves_pcm() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 30, true);
  segmenter.consume({300, std::vector<int16_t>(80, 2345)}, true);
  segmenter.finish("stop");
  auto metadata = output.events("audio.chunk_ready")[0]["data"];
  SF_INFO info{};
  auto samples = read_wav(output.root / metadata["wav_path"].get<std::string>(), info);
  check(info.frames == 4880 && samples.back() == 2345, "Stop must preserve the final partial PCM frame");
  check(metadata["end_ms"] == 305, "Partial frame must retain its actual duration");
}

void counters_survive_audio_format_change() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  {
    AudioSegmenter first("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
    feed(first, 0, 50, true);
    first.finish("format_changed");
  }
  {
    AudioSegmenter second("user-1-join-1", "Alice", 1, 32000, 1, journal, wav, output.recordings, utterances, chunks);
    for (int i = 0; i < 50; ++i) second.consume({500 + i * 10, std::vector<int16_t>(320, 1234)}, true);
    second.finish("stop");
  }
  auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 2 && ready[0]["data"]["chunk_id"] != ready[1]["data"]["chunk_id"],
        "Format change must not reuse chunk IDs");
  check(ready[1]["data"]["sequence"] == 2 && ready[1]["data"]["sample_rate"] == 32000,
        "Participant chunk sequence must survive format change");
}

void silence_and_invalid_vad_input() {
  SpeechDetector detector(16000);
  check(!detector.voiced(std::vector<int16_t>(160, 0), 1), "Silent mono must not trigger speech");
  check(!detector.voiced(std::vector<int16_t>(320, 0), 2), "Silent stereo must not trigger speech");
  bool rejected = false;
  try { detector.voiced(std::vector<int16_t>(12, 0), 1); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "Malformed VAD frame must be rejected");
}

void overlapping_speakers_keep_distinct_audio() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t u1 = 0, c1 = 0, u2 = 0, c2 = 0;
  AudioSegmenter first("user-1-join-1", "Alice", 1, 16000, 1, journal, wav, output.recordings, u1, c1);
  AudioSegmenter second("user-2-join-1", "Bob", 2, 16000, 1, journal, wav, output.recordings, u2, c2);
  for (int i = 0; i < 50; ++i) {
    feed(first, i * 10, 1, true, 1000);
    feed(second, i * 10, 1, true, -2000);
  }
  first.finish("stop");
  second.finish("stop");
  auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 2, "Overlapping speakers need independent chunks");
  for (size_t i = 0; i < ready.size(); ++i) {
    SF_INFO info{};
    auto samples = read_wav(output.root / ready[i]["data"]["wav_path"].get<std::string>(), info);
    check(std::all_of(samples.begin(), samples.end(), [&](int16_t value) { return value == (i == 0 ? 1000 : -2000); }),
          "Speakers' PCM must never be mixed");
    check(ready[i]["data"]["wav_path"].get<std::string>().find(i == 0 ? "recording-1__Alice__" : "recording-1__Bob__") == 0,
          "Each speaker's WAV must identify its display name in the filename");
    check(std::filesystem::path(ready[i]["data"]["wav_path"].get<std::string>()).parent_path().empty(),
          "All speakers' WAV files must be stored together in the meeting directory");
  }
}

void timestamp_filename_uses_shared_clock_and_sequence() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-42-join-1", "홍길동", 42, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 3723000, 5, true);
  segmenter.finish("stop");
  feed(segmenter, 3723050, 5, true);
  segmenter.finish("stop");
  auto ready = output.events("audio.chunk_ready");
  check(ready[0]["data"]["wav_path"] == "recording-1__홍길동__01:02:03-01:02:03__chunk-1.wav",
        "Filename must format hours, minutes and seconds from the recording timeline");
  check(ready[1]["data"]["wav_path"] == "recording-1__홍길동__01:02:03-01:02:03__chunk-2.wav",
        "Two subsecond utterances in the same second must not overwrite each other");
}

void real_vad_framing_rejoin_and_inactivity() {
  SF_INFO source_info{};
  auto source = read_wav(FVAD_FIXTURE, source_info);
  check(source_info.samplerate == 16000 && source_info.channels == 1, "Unexpected VAD fixture format");
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  AudioPipeline pipeline(journal, wav, output.recordings);
  pipeline.joined(42, "Speaker", 0);
  // Irregular 5 ms packets exercise buffering across callback boundaries.
  size_t count = std::min(source.size(), static_cast<size_t>(16000 * 3));
  count -= count % 80;
  for (size_t offset = 0; offset < count; offset += 80) {
    pipeline.consume({42, 16000, 1, static_cast<int64_t>(offset / 16),
                      std::vector<int16_t>(source.begin() + offset, source.begin() + offset + 80)});
    pipeline.consume({7, 16000, 1, static_cast<int64_t>(offset / 16), std::vector<int16_t>(80, 0)});
  }
  pipeline.advance(count / 16 + 400);
  check(!output.events("speech_on").empty(), "Real WebRTC VAD must detect voice fixture");
  auto before = output.events("audio.chunk_ready");
  check(!before.empty(), "Inactivity must flush pending voice without silent callbacks");
  for (const auto& event : before) check(event["data"]["user_id"] == 42, "Silent speaker must not get voice chunks");
  pipeline.left(42, count / 16 + 400);
  pipeline.joined(42, "Speaker", count / 16 + 500);
  pipeline.consume({42, 16000, 1, static_cast<int64_t>(count / 16 + 500), std::vector<int16_t>(160, 0)});
  pipeline.finish("stop", count / 16 + 510);
  auto joined = output.events("participant.joined");
  check(joined[0]["data"]["participant_session_id"] != joined.back()["data"]["participant_session_id"],
        "Rejoin must create a new participant session");
  check(output.events("speech_off").size() == output.events("speech_on").size(), "Every utterance must close");
}

void timestamp_gap_is_explicit_and_backward_time_fails() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  AudioPipeline pipeline(journal, wav, output.recordings);
  pipeline.consume({1, 16000, 1, 0, std::vector<int16_t>(160, 0)});
  pipeline.consume({1, 16000, 1, 100, std::vector<int16_t>(160, 0)});
  check(output.events("audio.gap").size() == 1, "Missing audio interval must not silently concatenate");
  bool rejected = false;
  try { pipeline.consume({1, 16000, 1, 50, std::vector<int16_t>(160, 0)}); }
  catch (const std::runtime_error&) { rejected = true; }
  check(rejected && output.events("audio.out_of_order").size() == 1, "Backward timestamp must fail visibly");
  pipeline.finish("stop", 110);
}

void recording_origins_and_duplicate_statuses() {
  Output output;
  output.recordings = RecordingTimeline{};
  check(output.recordings.change(0, 2000, -60000, 1700000000000), "Initial recording state must be accepted");
  check(output.recordings.spans().size() == 1, "Initial snapshot must not create overlapping unrecorded PCM");
  check(!output.recordings.change(0, 2000, -60000, 1700000000000), "Repeated start must not reset the recording");
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-42-join-1", "홍길동", 42, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 10000, 50, true);
  segmenter.finish("silence");
  output.recordings.change(1, 3540000, -60000, 1700000000000);
  segmenter.recording_changed("cloud_recording_stopped", 3540000);
  output.recordings.change(0, 3540000, -60000, 1700000000000);
  segmenter.recording_changed("cloud_recording_started", 3540000);
  feed(segmenter, 3550000, 50, true);
  segmenter.finish("stop");
  const auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 2 && ready[0]["data"]["start_ms"] == 70000,
        "Late admission must retain the meeting API origin for the initial cloud file");
  check(ready[1]["data"]["recording_id"] == "recording-2" && ready[1]["data"]["start_ms"] == 10000,
        "Second recording's ten seconds must be 00:00:10, never 01:00:10");
  check(ready[1]["data"]["wav_path"] == "recording-2__홍길동__00:00:10-00:00:10__chunk-2.wav",
        "Readable filename must identify cloud file, speaker and playback position");
  check(ready[1]["data"]["capture_start_ms"] == 3550000 &&
        ready[1]["data"]["timestamp_origin"] == "sdk.cloud_recording_callback",
        "Preserve monotonic capture time and the chosen origin source separately");
  check(output.events("speech_on")[1]["data"]["speech_start_ms"] == 10000,
        "Speech events must also report cloud playback time");
}

void cloud_boundary_splits_pcm_without_fake_speech_events() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter segmenter("user-42-join-1", "홍길동", 42, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(segmenter, 0, 100, true);
  output.recordings.change(1, 995, 0, 0);
  segmenter.recording_changed("cloud_recording_stopped", 995);
  output.recordings.change(0, 995, 0, 0);
  segmenter.recording_changed("cloud_recording_started", 995);
  // Already buffered PCM past the boundary must wait for the next recording state.
  feed(segmenter, 1000, 100, true, 2345);
  segmenter.finish("silence");
  const auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 2 && ready[0]["data"]["end_ms"] == 995 && ready[1]["data"]["start_ms"] == 0,
        "Cloud boundaries must clip PCM at sample precision and reset the new file");
  check(ready[0]["data"]["utterance_id"] == ready[1]["data"]["utterance_id"] &&
        output.events("speech_on").size() == 1 && output.events("speech_off").size() == 1,
        "A file boundary must preserve the utterance without invented VAD transitions");
  SF_INFO info{};
  auto first = read_wav(output.root / ready[0]["data"]["wav_path"].get<std::string>(), info);
  check(first.size() == 995 * 16 && first.back() == 1234, "First file must exclude PCM after stop");
  auto second = read_wav(output.root / ready[1]["data"]["wav_path"].get<std::string>(), info);
  check(second.size() == 1005 * 16 && second.front() == 1234 && second[80] == 2345,
        "Second file must preserve buffered boundary PCM and all later samples exactly once");
}

void pause_resumes_same_playback_clock_and_names_stay_safe() {
  Output output;
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0, u2 = 0, c2 = 0;
  AudioSegmenter speaker("user-1-join-1", "홍길동/../\n", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(speaker, 0, 100, true);
  output.recordings.change(3, 1000, 0, 0);
  speaker.recording_changed("cloud_recording_paused", 1000);
  feed(speaker, 1000, 1000, true);
  speaker.recording_changed("paused_pcm", 11000);
  check(output.events("audio.chunk_ready").size() == 2 &&
        output.events("audio.chunk_ready")[1]["data"]["recording_id"].is_null() &&
        output.events("audio.chunk_ready")[1]["data"]["timestamp_origin"] == "wall_clock_gmt9",
        "Paused audio must be preserved with wall-clock timestamps rather than cloud positions");
  output.recordings.change(0, 11000, 0, 0);
  speaker.recording_changed("cloud_recording_started", 11000);
  feed(speaker, 11000, 100, true);
  speaker.finish("silence");
  const auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 3 && ready[2]["data"]["recording_id"] == "recording-1" &&
        ready[2]["data"]["start_ms"] == 1000 && ready[2]["data"]["end_ms"] == 2000,
        "Resume must subtract the pause and continue the same cloud file");
  check(ready[0]["data"]["display_name"] == "홍길동/../\n" &&
        ready[0]["data"]["speaker_label"] == "홍길동_..__",
        "Metadata must retain the original name while the filename sanitizes paths and controls");
  AudioSegmenter other("user-2-join-1", "홍길동/../\n", 2, 16000, 1, journal, wav, output.recordings, u2, c2);
  feed(other, 11000, 100, true);
  other.finish("silence");
  const auto all = output.events("audio.chunk_ready");
  check(all.size() == 4 && all[3]["data"]["wav_path"] != all[2]["data"]["wav_path"] &&
        all[3]["data"]["file_sequence"] == 4, "Identical display names must never overwrite WAVs");
  speaker.renamed("새 이름");
  feed(speaker, 12000, 100, true);
  speaker.finish("stop");
  check(output.events("audio.chunk_ready").back()["data"]["wav_path"].get<std::string>().find("recording-1__새 이름__") == 0,
        "Renamed speakers must use their new display name");
  std::string long_name;
  for (int i = 0; i < 100; ++i) long_name += "홍";
  const auto label = WavWriter::speaker_label(long_name);
  check(label.size() == 78 && Json(label).dump().size() > 0, "Long Korean names must be shortened without breaking UTF-8");
}

void unrecorded_wall_clock_wraps_midnight_without_losing_the_date() {
  Output output;
  output.recordings = RecordingTimeline{};
  output.recordings.change(1, 0, 0, 53998000);  // UTC 14:59:58 = GMT+9 23:59:58.
  EventJournal journal(output.root, "session");
  WavWriter wav(output.root);
  uint64_t utterances = 0, chunks = 0;
  AudioSegmenter speaker("user-1-join-1", "홍길동", 1, 16000, 1, journal, wav, output.recordings, utterances, chunks);
  feed(speaker, 0, 500, true);
  speaker.finish("stop");
  const auto ready = output.events("audio.chunk_ready");
  check(ready.size() == 1 && ready[0]["data"]["wav_path"] ==
        "unrecorded__홍길동__23:59:58-00:00:03__chunk-1.wav",
        "Unrecorded filenames must use a 24-hour GMT+9 clock, including midnight wrap");
  check(ready[0]["data"]["start_unix_ms"] == 53998000 && ready[0]["data"]["end_unix_ms"] == 54003000 &&
        ready[0]["data"]["start_ms"] == 53998000 && ready[0]["data"]["end_ms"] == 54003000,
        "Absolute milliseconds must preserve the date and ordering across midnight");
  check(output.events("speech_on")[0]["data"]["recording_time_ms"].is_null() &&
        output.events("speech_on")[0]["data"]["speech_start_unix_ms"] == 53998000,
        "Unrecorded speech events must retain absolute time without inventing a cloud position");
}

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"silence boundary and pre-roll", silence_boundary_and_pre_roll},
    {"long speech publishes during utterance", long_speech_publishes_during_utterance},
    {"forced boundary has no duplicate-only tail", exact_forced_boundary_avoids_duplicate_only_chunk},
    {"30 s limit excludes pre-roll and silence", thirty_seconds_counts_speech_without_pre_roll_or_silence_tail},
    {"partial PCM frame", final_partial_frame_preserves_pcm},
    {"format change counters", counters_survive_audio_format_change},
    {"silence and invalid VAD input", silence_and_invalid_vad_input},
    {"overlapping speakers", overlapping_speakers_keep_distinct_audio},
    {"participant timestamp filenames", timestamp_filename_uses_shared_clock_and_sequence},
    {"real VAD framing, rejoin and inactivity", real_vad_framing_rejoin_and_inactivity},
    {"timestamp gap and backward time", timestamp_gap_is_explicit_and_backward_time_fails},
    {"cloud recording origins", recording_origins_and_duplicate_statuses},
    {"cloud boundary PCM and speech events", cloud_boundary_splits_pcm_without_fake_speech_events},
    {"cloud pause and readable speaker names", pause_resumes_same_playback_clock_and_names_stay_safe},
    {"unrecorded GMT+9 midnight", unrecorded_wall_clock_wraps_midnight_without_losing_the_date}
  };
  for (const auto& [name, test] : tests) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
  }
  return 0;
}

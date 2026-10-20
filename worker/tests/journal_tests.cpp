#include "zoom_bot/capture_runtime.hpp"

#include <atomic>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace zoom_bot;
namespace {
std::atomic<int> sync_calls{0};
std::atomic<bool> fail_sync{false};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Output {
  std::filesystem::path root;
  Output() { char path[] = "/tmp/zoom-bot-journal-XXXXXX"; root = mkdtemp(path); }
  ~Output() { std::filesystem::remove_all(root); }
};
}
// Count synchronization and inject storage failures without changing production I/O APIs.
extern "C" int fsync(int) {
  ++sync_calls;
  if (fail_sync) { errno = EIO; return -1; }
  return 0;
}

int main() {
  try {
    {
      Output output;
      EventJournal journal(output.root, "session");
      for (int i = 0; i < 1000; ++i) journal.append("sdk.callback", i, {{"index", i}});
      require(sync_calls < 100, "A burst must not sync every raw packet");
      std::ifstream file(output.root / "events.jsonl");
      int rows = 0;
      for (std::string line; std::getline(file, line);) {
        const auto event = nlohmann::json::parse(line);
        require(event["seq"] == rows + 1 && event["data"]["index"] == rows,
                "All events must be immediately readable in their original order");
        ++rows;
      }
      require(rows == 1000, "Batching sync must never drop or defer journal lines");
      journal.sync();
      const int synced = sync_calls;
      journal.sync();
      require(sync_calls == synced, "An unchanged journal must not sync repeatedly");
      journal.append("sdk.callback", 1000, {});
      std::this_thread::sleep_for(std::chrono::milliseconds(110));
      journal.sync_if_due();
      require(sync_calls == synced + 1, "An idle dirty journal must sync when its deadline passes");
      journal.append("sdk.callback", 1100, {});
      fail_sync = true;
      bool rejected = false;
      try { journal.sync(); } catch (const std::runtime_error&) { rejected = true; }
      require(rejected, "A failed sync must not report success");
      fail_sync = false;
      const int failed = sync_calls;
      journal.sync();
      require(sync_calls == failed + 1, "A failed sync must keep pending data dirty for retry");
    }
    {
      Output output;
      const int before = sync_calls;
      CaptureRuntime capture(output.root, "idle");
      capture.sdk_callback("IMeetingServiceEvent", "onMeetingStatusChanged", {{"status", 3}});
      for (int i = 0; i < 50 && sync_calls == before; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      require(sync_calls > before, "Capture must sync idle events without waiting for another callback or shutdown");
      capture.close();
      require(!capture.failed(), "Idle journal synchronization must preserve capture");
    }
    {
      Output output;
      CaptureRuntime capture(output.root, "failed_shutdown");
      capture.sdk_callback("IMeetingServiceEvent", "onMeetingStatusChanged", {{"status", 7}});
      fail_sync = true;
      capture.close();
      require(capture.failed(), "A final sync failure must fail capture rather than report successful shutdown");
      fail_sync = false;
    }
    std::cout << "Journal ordering, sync batching, idle deadlines and storage failures passed\n";
    return 0;
  } catch (const std::exception& error) { fail_sync = false; std::cerr << error.what() << '\n'; return 1; }
}

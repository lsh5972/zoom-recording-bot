#include "zoom_bot/zoom_session.hpp"

#include <glib.h>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
}

int main(int argc, char** argv) {
  if (argc != 5 || std::string(argv[1]) != "--config" || std::string(argv[3]) != "--output") {
    std::cerr << "Usage: zoom-bot-worker --config JOIN_JSON --output DIRECTORY\n";
    return 2;
  }
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGINT, signal_handler);
  try {
    auto config = zoom_bot::JoinConfig::read(argv[2]);
    zoom_bot::CaptureRuntime capture(argv[4], config.session_id, 32 * 1024 * 1024, config.meeting_start_unix_ms);
    bool failed = false;
    {
      zoom_bot::ZoomSession session(std::move(config), capture);
      try {
        session.start();
        while (!session.done()) {
          // Bound each iteration so a busy SDK event source cannot starve SIGTERM or queue failure checks.
          for (int i = 0; i < 100 && g_main_context_pending(nullptr); ++i) g_main_context_iteration(nullptr, false);
          if (interrupted) session.stop("signal");
          session.tick();
          g_usleep(10000);
        }
        failed = session.failed_session();
      } catch (...) {
        capture.event("worker.error", {{"reason", "sdk_start_failed"}});
        session.stop("sdk_start_failed");
        failed = true;
      }
    }  // SDK teardown completes before the capture consumer closes.
    capture.close();
    return failed || capture.failed() ? 1 : 0;
  } catch (...) {
    std::cerr << "Worker failed; check configuration and events.jsonl\n";
    return 1;
  }
}

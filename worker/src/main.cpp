#include "zoom_bot/zoom_session.hpp"

#include <glib.h>
#include <csignal>
#include <iostream>
#include <memory>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }

struct SessionLoop {
  zoom_bot::ZoomSession& session;
  GMainLoop* loop;
  bool started = false, failed = false;
};

gboolean update_session(gpointer data) {
  auto& state = *static_cast<SessionLoop*>(data);
  try {
    if (!state.started) { state.started = true; state.session.start(); }
    if (interrupted) state.session.stop("signal");
    state.session.tick();
  } catch (...) {
    state.failed = true;
    std::cerr << "Worker failed: SDK start failed\n";
    state.session.stop("sdk_start_failed");
  }
  if (state.failed || state.session.done()) g_main_loop_quit(state.loop);
  return G_SOURCE_CONTINUE;
}
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
      std::unique_ptr<GMainLoop, decltype(&g_main_loop_unref)> loop(g_main_loop_new(nullptr, false), g_main_loop_unref);
      SessionLoop state{session, loop.get()};
      // Keep GLib dispatch and SDK calls on the main thread, including authentication and join.
      const auto timer = g_timeout_add(10, update_session, &state);
      g_main_loop_run(loop.get());
      g_source_remove(timer);
      failed = state.failed || session.failed_session();
    }  // SDK teardown completes before the capture consumer closes.
    capture.close();
    return failed || capture.failed() ? 1 : 0;
  } catch (...) {
    std::cerr << "Worker failed; check configuration and events.jsonl\n";
    return 1;
  }
}

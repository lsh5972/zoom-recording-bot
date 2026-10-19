#include "zoom_bot/zoom_session.hpp"

#include <glib.h>
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
enum class Scenario { callback, signal, start_failure };
struct Probe {
  Scenario scenario;
  std::thread::id main_thread = std::this_thread::get_id();
  std::atomic<bool> callback_received{false}, callback_on_main{false};
  bool calls_on_main = true, context_owned = true, stopped = false;
  int busy_dispatches = 0;
  void observe() {
    calls_on_main = calls_on_main && std::this_thread::get_id() == main_thread;
    context_owned = context_owned && g_main_context_is_owner(g_main_context_default());
  }
};
Probe* probe = nullptr;
}

namespace zoom_bot {
// Exercise the real worker entry point and GLib; replace only the Zoom session.
class MainLoopTestSession {
 public:
  MainLoopTestSession(JoinConfig, CaptureRuntime&) {}
  ~MainLoopTestSession() {
    if (worker_.joinable()) worker_.join();
    if (busy_source_) g_source_remove(busy_source_);
  }
  void start() {
    probe->observe();
    if (probe->scenario == Scenario::start_failure) throw std::runtime_error("Test SDK start failure");
    if (probe->scenario == Scenario::signal) {
      // A continuously ready SDK source must not starve the stop timer.
      busy_source_ = g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer) -> gboolean {
        ++probe->busy_dispatches;
        return G_SOURCE_CONTINUE;
      }, nullptr, nullptr);
      g_timeout_add(25, [](gpointer) -> gboolean { std::raise(SIGTERM); return G_SOURCE_REMOVE; }, nullptr);
    } else {
      worker_ = std::thread([] {
        g_main_context_invoke(nullptr, [](gpointer) -> gboolean {
          probe->callback_on_main = std::this_thread::get_id() == probe->main_thread;
          probe->callback_received = true;
          return G_SOURCE_REMOVE;
        }, nullptr);
      });
    }
  }
  void tick() {
    probe->observe();
    if (probe->callback_received) done_ = true;
  }
  bool done() const { return done_; }
  bool failed_session() const { return false; }
  void stop(const std::string&) { probe->observe(); probe->stopped = true; done_ = true; }

 private:
  bool done_ = false;
  guint busy_source_ = 0;
  std::thread worker_;
};
}

#define ZoomSession MainLoopTestSession
#define main zoom_bot_worker_main
#include "../src/main.cpp"
#undef main
#undef ZoomSession

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Output {
  std::filesystem::path root;
  Output() {
    char path[] = "/tmp/zoom-bot-main-XXXXXX";
    root = mkdtemp(path);
    std::ofstream(root / "join.json") << nlohmann::json{
      {"schema_version", 1}, {"session_id", "00000000-0000-0000-0000-000000000001"},
      {"meeting_id", "12345678901"}, {"host_user_id", "test-host"}, {"passcode", ""},
      {"display_name", "Test bot"}, {"sdk_jwt", "test-jwt"}, {"user_zak", "test-zak"},
      {"meeting_start_unix_ms", 1700000000000}};
  }
  ~Output() { std::filesystem::remove_all(root); }
};
}

int main() {
  try {
    for (auto scenario : {Scenario::callback, Scenario::signal, Scenario::start_failure}) {
      Output output;
      Probe state{scenario};
      probe = &state;
      interrupted = 0;
      std::string executable = "worker", config_flag = "--config", output_flag = "--output";
      auto config = (output.root / "join.json").string(), directory = (output.root / "output").string();
      char* args[] = {executable.data(), config_flag.data(), config.data(), output_flag.data(), directory.data()};
      const auto result = zoom_bot_worker_main(5, args);
      require(state.calls_on_main, "SDK start, tick and stop must stay on the main thread");
      require(state.context_owned, "SDK calls must run while the main thread owns the GLib context");
      require(result == (scenario == Scenario::start_failure ? 1 : 0), "Worker must preserve its exit status");
      if (scenario == Scenario::callback)
        require(state.callback_received && state.callback_on_main, "A background GLib invocation must reach the main thread");
      if (scenario == Scenario::signal)
        require(state.stopped && state.busy_dispatches > 0, "SIGTERM must stop the worker with a continuously ready SDK source");
      if (scenario == Scenario::start_failure)
        require(state.stopped, "A startup exception must stop the session without escaping the C callback");
      require(!g_main_context_pending(nullptr), "Worker exit must remove its timer and callback state");
    }
    std::cout << "3 worker main-loop scenarios passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

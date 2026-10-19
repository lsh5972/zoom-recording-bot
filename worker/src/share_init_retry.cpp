#include "zoom_bot/share_init_retry.hpp"

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

namespace {
constexpr char init_symbol[] =
    "_ZN7ZOOMSDK18CoreShareEventSink4InitEPNS_15CMeetingServiceEN10ZMMSDKCORE22MSDKCoreShareEventTypeE";
constexpr char uninit_symbol[] = "_ZN7ZOOMSDK18CoreShareEventSink6UninitEv";
using Init = void (*)(void*, void*, int);
using Uninit = void (*)(void*);
struct Binding { void* sink = nullptr; void* meeting = nullptr; Init init = nullptr; };
Binding binding;
std::mutex binding_mutex;

template <typename Function> Function next(const char* symbol) {
  auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, symbol));
  if (!function) {
    std::fputs("Share initialization shim could not resolve the original SDK function\n", stderr);
    std::abort();
  }
  return function;
}

bool registered(void* sink) {
  // CoreShareEventSink::Init/Uninit read and write this byte in ARM64 7.2.1.5860.
  // CMake verifies the exact SDK SHA256 before compiling this library.
  return static_cast<const unsigned char*>(sink)[0x15] != 0;
}
}

extern "C" void intercept_share_init(void*, void*, int)
    asm("_ZN7ZOOMSDK18CoreShareEventSink4InitEPNS_15CMeetingServiceEN10ZMMSDKCORE22MSDKCoreShareEventTypeE");
extern "C" void intercept_share_init(void* sink, void* meeting, int type) {
  static const auto original = next<Init>(init_symbol);
  original(sink, meeting, type);
  if (type == 0) {  // Main conference only; do not redirect breakout-room delegates.
    std::lock_guard<std::mutex> lock(binding_mutex);
    binding = {sink, meeting, original};
  }
}

extern "C" void intercept_share_uninit(void*)
    asm("_ZN7ZOOMSDK18CoreShareEventSink6UninitEv");
extern "C" void intercept_share_uninit(void* sink) {
  static const auto original = next<Uninit>(uninit_symbol);
  {
    std::lock_guard<std::mutex> lock(binding_mutex);
    if (binding.sink == sink) binding = {};
  }
  original(sink);
}

extern "C" ShareInitRetryResult zoom_bot_retry_share_init(void* meeting) {
  Binding current;
  {
    std::lock_guard<std::mutex> lock(binding_mutex);
    current = binding;
  }
  if (!current.sink || !meeting || current.meeting != meeting) return {};
  // Called by the SDK main thread while this meeting is admitted and recording.
  const bool before = registered(current.sink);
  current.init(current.sink, current.meeting, 0);
  return {true, true, before, registered(current.sink)};
}

extern "C" bool zoom_bot_share_init_captured(void* meeting) {
  std::lock_guard<std::mutex> lock(binding_mutex);
  return binding.sink && meeting && binding.meeting == meeting;
}

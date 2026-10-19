#include <array>

namespace {
std::array<unsigned char, 24> sink{};
bool ready = false;
int calls = 0;
}

extern "C" void fixture_init(void*, void*, int)
    asm("_ZN7ZOOMSDK18CoreShareEventSink4InitEPNS_15CMeetingServiceEN10ZMMSDKCORE22MSDKCoreShareEventTypeE");
extern "C" void fixture_init(void* self, void*, int) {
  ++calls;
  static_cast<unsigned char*>(self)[0x15] = ready;
}
extern "C" void fixture_uninit(void*) asm("_ZN7ZOOMSDK18CoreShareEventSink6UninitEv");
extern "C" void fixture_uninit(void* self) { static_cast<unsigned char*>(self)[0x15] = false; }

extern "C" void fixture_start(void* meeting, int type) { fixture_init(sink.data(), meeting, type); }
extern "C" void fixture_stop() { fixture_uninit(sink.data()); }
extern "C" void fixture_ready(bool value) { ready = value; }
extern "C" int fixture_calls() { return calls; }
extern "C" bool fixture_registered() { return sink[0x15] != 0; }

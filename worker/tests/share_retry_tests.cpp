#include "zoom_bot/share_init_retry.hpp"
#include <iostream>
#include <stdexcept>

extern "C" void fixture_start(void*, int);
extern "C" void fixture_stop();
extern "C" void fixture_ready(bool);
extern "C" int fixture_calls();
extern "C" bool fixture_registered();

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main() {
  try {
    int meeting = 0, other = 0;
    require(!zoom_bot_retry_share_init(&meeting).captured, "Never invoke an uncaptured SDK object");
    fixture_start(&meeting, 0);
    require(zoom_bot_share_init_captured(&meeting) && !zoom_bot_share_init_captured(&other),
            "Capture only the original main conference object");
    require(fixture_calls() == 1 && !fixture_registered(), "Forward early initialization unchanged");
    require(!zoom_bot_retry_share_init(&other).invoked && fixture_calls() == 1,
            "Never invoke a binding belonging to another meeting");
    auto result = zoom_bot_retry_share_init(&meeting);
    require(result.captured && result.invoked && !result.registered_before && !result.registered_after,
            "Report a failed retry without claiming registration succeeded");
    fixture_ready(true);
    result = zoom_bot_retry_share_init(&meeting);
    require(result.invoked && !result.registered_before && result.registered_after && fixture_registered(),
            "Retry the original initialization after the helper becomes ready");
    result = zoom_bot_retry_share_init(&meeting);
    require(result.registered_before && result.registered_after && fixture_calls() == 4,
            "Reinitialization must use the original function without recursive interception");
    fixture_stop();
    require(!fixture_registered() && !zoom_bot_share_init_captured(&meeting) && !zoom_bot_retry_share_init(&meeting).invoked,
            "Uninitialization must invalidate the saved SDK object");
    fixture_start(&other, 1);
    require(!zoom_bot_retry_share_init(&other).captured, "Never retry a breakout-room delegate");
    fixture_stop();
    std::cout << "Share initialization interposition tests passed (fake SDK)\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#pragma once

// Private SDK ABI: enabled only for the pinned Linux ARM64 experimental build.
struct ShareInitRetryResult {
  bool captured = false;
  bool invoked = false;
  bool registered_before = false;
  bool registered_after = false;
};

extern "C" ShareInitRetryResult zoom_bot_retry_share_init(void* meeting);
extern "C" bool zoom_bot_share_init_captured(void* meeting);

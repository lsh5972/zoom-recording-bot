#!/bin/sh
set -eu

# Apply interposition only to the SDK worker, not PulseAudio, dbus, or setup tools.
export LD_PRELOAD=/app/libzoom_share_init_retry.so
exec /app/zoom-bot-worker-bin "$@"

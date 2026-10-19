#!/bin/sh
set -eu
umask 077

if [ "$#" -eq 4 ] && [ "$1" = --config ] && [ "$3" = --output ]; then
  diagnostic_output=$(mkdir -p "$4/sdk-logs" && cd "$4/sdk-logs" && pwd -P)
  # Both paths occur in Linux SDK releases. Persist only logs, never the SDK's account data.
  for sdk_directory in /root/.zoomsdk /root/zoomsdk; do
    mkdir -p "$sdk_directory"
    ln -s "$diagnostic_output" "$sdk_directory/logs"
  done
fi

exec /app/entrypoint.sh "$@"

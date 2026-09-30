#!/bin/sh
set -eu
umask 077

mkdir -p /run/dbus /root/.config
dbus-uuidgen --ensure=/etc/machine-id
dbus-daemon --system --fork
pulseaudio --daemonize=yes --exit-idle-time=-1 --log-target=stderr
pactl load-module module-null-sink sink_name=BotSpeaker channels=1 rate=32000 >/dev/null
pactl load-module module-null-source source_name=BotMicrophone channels=1 rate=32000 >/dev/null
pactl set-default-sink BotSpeaker
pactl set-default-source BotMicrophone
printf '[General]\nsystem.audio.type=default\n' > /root/.config/zoomus.conf

# Native process receives Docker SIGTERM directly and drains pending speech/WAVs.
exec /app/zoom-bot-worker "$@"

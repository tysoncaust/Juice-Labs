#!/bin/sh
set -eu
MOONLIGHT="/Applications/Moonlight.app/Contents/MacOS/Moonlight"
if [ ! -x "$MOONLIGHT" ]; then
  osascript -e 'display dialog "Moonlight is not installed in /Applications." buttons {"OK"} default button "OK" with icon stop'
  exit 1
fi
exec "$MOONLIGHT" stream --1080 --fps 60 --bitrate 15000 --video-codec H.264 --video-decoder hardware --display-mode fullscreen --no-quit-after Laptop "Desktop"

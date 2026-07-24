#!/bin/sh
set -eu

PORT="${1:-50000}"
DURATION="${2:-45}"
EVIDENCE_DIR="${3:-/private/tmp/rgpu-compatibility}"
VLC="/Applications/VLC.app/Contents/MacOS/VLC"

if [ ! -x "$VLC" ]; then
  echo "MAC_LAN_VIDEO_RECEIVER=FAIL reason=vlc_missing"
  exit 2
fi

mkdir -p "$EVIDENCE_DIR"
stamp=$(date -u +%Y%m%d-%H%M%S)
log="$EVIDENCE_DIR/vlc-receiver-$stamp.log"
summary="$EVIDENCE_DIR/vlc-receiver-$stamp.txt"

"$VLC" -I dummy --vout dummy --aout dummy \
  --no-video-title-show --network-caching=50 \
  --clock-jitter=0 --clock-synchro=0 --demux=ts -vvv \
  "udp://@:$PORT" >/dev/null 2>"$log" &
pid=$!

sleep "$DURATION"
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

h264=$(grep -Eic 'h264|avcodec.*264|codec.*264' "$log" || true)
ts=$(grep -Eic 'ts demux|using demux module.*ts|MPEG-TS|mpegts' "$log" || true)
video=$(grep -Eic 'video output|vout display|decoded video|picture to display|using video decoder' "$log" || true)
data=$(grep -Eic 'received first data|pre-buffering done|PCR|continuity|packet' "$log" || true)

if [ "$h264" -gt 0 ] && [ "$ts" -gt 0 ] && [ "$video" -gt 0 ]; then
  pass=PASS
else
  pass=FAIL
fi

{
  echo "MAC_LAN_VIDEO_RECEIVER=$pass"
  echo "LISTEN=udp://@:$PORT"
  echo "PLAYER=VLC"
  echo "MPEG_TS_MARKERS=$ts"
  echo "H264_LOG_MARKERS=$h264"
  echo "VIDEO_OUTPUT_MARKERS=$video"
  echo "DATA_MARKERS=$data"
  echo "LOG=$log"
} >"$summary"
cat "$summary"
[ "$pass" = PASS ]

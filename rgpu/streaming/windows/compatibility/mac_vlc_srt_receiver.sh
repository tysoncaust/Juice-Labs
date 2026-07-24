#!/bin/sh
set -eu

HOST=${1:?usage: mac_vlc_srt_receiver.sh HOST [PORT] [TIMEOUT_SECONDS] EVIDENCE_JSON [EXPECT_AUTH_FAILURE]}
PORT=${2:-51001}
TIMEOUT_SECONDS=${3:-20}
EVIDENCE_JSON=${4:?evidence JSON path is required}
EXPECT_AUTH_FAILURE=${5:-0}
VLC=${VLC:-/Applications/VLC.app/Contents/MacOS/VLC}
PASSPHRASE=${RGPU_SRT_PASSPHRASE:-}

case ${#PASSPHRASE} in
  0|1|2|3|4|5|6|7|8|9) echo 'RGPU_SRT_PASSPHRASE must contain 10-79 characters' >&2; exit 2 ;;
esac
[ ${#PASSPHRASE} -le 79 ] || { echo 'RGPU_SRT_PASSPHRASE must contain 10-79 characters' >&2; exit 2; }
[ -x "$VLC" ] || { echo "VLC not found: $VLC" >&2; exit 2; }

FIFO=$(mktemp -u /private/tmp/rgpu-vlc-srt-fifo.XXXXXX)
RAW_LOG=$(mktemp /private/tmp/rgpu-vlc-srt-raw.XXXXXX)
mkdir -p "$(dirname "$EVIDENCE_JSON")"
mkfifo "$FIFO"
VLC_PID=''
STAMP_PID=''
cleanup() {
  if [ -n "$VLC_PID" ] && kill -0 "$VLC_PID" 2>/dev/null; then kill "$VLC_PID" 2>/dev/null || true; fi
  if [ -n "$STAMP_PID" ] && kill -0 "$STAMP_PID" 2>/dev/null; then kill "$STAMP_PID" 2>/dev/null || true; fi
  rm -f "$FIFO" "$RAW_LOG"
}
trap cleanup EXIT INT TERM

START_EPOCH=$(python3 -c 'import time; print(f"{time.time():.9f}")')
"$VLC" -I dummy --verbose=2 --vout dummy --aout dummy --no-video-title-show \
  --latency=60 --passphrase="$PASSPHRASE" "srt://$HOST:$PORT" >"$FIFO" 2>&1 &
VLC_PID=$!
perl -MTime::HiRes=time -ne 'printf "%.9f %s", time, $_' <"$FIFO" >"$RAW_LOG" &
STAMP_PID=$!

ELAPSED=0
while [ "$ELAPSED" -lt "$TIMEOUT_SECONDS" ]; do
  if ! kill -0 "$VLC_PID" 2>/dev/null; then break; fi
  sleep 1
  ELAPSED=$((ELAPSED + 1))
done
if kill -0 "$VLC_PID" 2>/dev/null; then kill "$VLC_PID" 2>/dev/null || true; fi
wait "$VLC_PID" 2>/dev/null || true
sleep 1
if kill -0 "$STAMP_PID" 2>/dev/null; then kill "$STAMP_PID" 2>/dev/null || true; fi
wait "$STAMP_PID" 2>/dev/null || true

python3 - "$RAW_LOG" "$EVIDENCE_JSON" "$START_EPOCH" "$EXPECT_AUTH_FAILURE" "$HOST" "$PORT" "$TIMEOUT_SECONDS" <<'PY'
from __future__ import annotations
import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

raw_path = Path(sys.argv[1])
evidence_path = Path(sys.argv[2]).expanduser().resolve()
start_epoch = float(sys.argv[3])
expect_failure = sys.argv[4] == "1"
host = sys.argv[5]
port = int(sys.argv[6])
timeout_seconds = int(sys.argv[7])
passphrase = os.environ["RGPU_SRT_PASSPHRASE"]
log_path = evidence_path.with_suffix(".log")

patterns = {
    "srt_access": re.compile(r'access_srt|srt connect|srt://', re.I),
    "mpeg_ts": re.compile(r'mpeg.?ts|transport stream|ts demux', re.I),
    "h264": re.compile(r'h264|avc1|\bavc\b', re.I),
    "aac": re.compile(r'\baac\b|mp4a', re.I),
    "video_pipeline": re.compile(r'video output|video decoder|\bvout\b', re.I),
    "audio_pipeline": re.compile(r'audio output|audio decoder|\baout\b', re.I),
    "incorrect_passphrase": re.compile(r'incorrect passphrase|encryption error|connection rejected', re.I),
}
counts = {key: 0 for key in patterns}
first = {key: None for key in patterns}
dimensions: list[tuple[int, int]] = []
safe_lines: list[str] = []
for raw in raw_path.read_text(encoding="utf-8", errors="replace").splitlines():
    match = re.match(r'^(\d+\.\d+)\s(.*)$', raw)
    if match:
        timestamp = float(match.group(1))
        line = match.group(2)
    else:
        timestamp = start_epoch
        line = raw
    safe_lines.append(f"{timestamp:.9f} {line.replace(passphrase, '<redacted>')}")
    for key, pattern in patterns.items():
        if pattern.search(line):
            counts[key] += 1
            if first[key] is None:
                first[key] = timestamp
    for width, height in re.findall(r'(?<!\d)(\d{3,5})x(\d{3,5})(?!\d)', line):
        w, h = int(width), int(height)
        if w >= 320 and h >= 240:
            dimensions.append((w, h))

log_path.write_text("\n".join(safe_lines) + "\n", encoding="utf-8")
decoded = all(counts[key] > 0 for key in ("h264", "aac", "video_pipeline", "audio_pipeline"))
if expect_failure:
    passed = counts["incorrect_passphrase"] > 0 and not decoded
else:
    passed = decoded and counts["incorrect_passphrase"] == 0
observed_mode = None
if dimensions:
    from collections import Counter
    observed_mode = Counter(dimensions).most_common(1)[0][0]
summary = {
    "schema": "rgpu-vlc-encrypted-srt-receiver-v1",
    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    "passed": passed,
    "expect_auth_failure": expect_failure,
    "transport": "SRT",
    "reliable_transport": True,
    "encryption": "AES-256 peer requested by sender; passphrase supplied to VLC",
    "passphrase_sha256_12": hashlib.sha256(passphrase.encode()).hexdigest()[:12],
    "host": host,
    "port": port,
    "timeout_seconds": timeout_seconds,
    "marker_counts": counts,
    "startup_ms": {
        key: round((value - start_epoch) * 1000.0, 3) if value is not None else None
        for key, value in first.items()
    },
    "observed_width": observed_mode[0] if observed_mode else None,
    "observed_height": observed_mode[1] if observed_mode else None,
    "video_retained": False,
    "game_process_access": False,
    "log": str(log_path),
}
evidence_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(f"VLC_ENCRYPTED_SRT_RECEIVER={'PASS' if passed else 'FAIL'}")
print(f"H264_MARKERS={counts['h264']}")
print(f"AAC_MARKERS={counts['aac']}")
print(f"VIDEO_PIPELINE_MARKERS={counts['video_pipeline']}")
print(f"AUDIO_PIPELINE_MARKERS={counts['audio_pipeline']}")
print(f"INCORRECT_PASSPHRASE_MARKERS={counts['incorrect_passphrase']}")
print(f"STARTUP_TO_H264_MS={summary['startup_ms']['h264']}")
print(f"STARTUP_TO_VIDEO_PIPELINE_MS={summary['startup_ms']['video_pipeline']}")
print(f"OBSERVED_MODE={summary['observed_width']}x{summary['observed_height']}")
print(f"EVIDENCE={evidence_path}")
raise SystemExit(0 if passed else 2)
PY

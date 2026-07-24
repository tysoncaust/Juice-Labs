#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/out"
mkdir -p "$OUT"
rm -f "$OUT"/phase2.vert.spv "$OUT"/phase2.frag.spv "$OUT"/frame.rgba "$OUT"/frame.h264 "$OUT"/phase2-evidence.json "$OUT"/phase2-summary.txt

glslangValidator -V "$ROOT/shaders/phase2.vert" -o "$OUT/phase2.vert.spv"
glslangValidator -V "$ROOT/shaders/phase2.frag" -o "$OUT/phase2.frag.spv"

g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers "$ROOT/phase2_executor.cpp" -o "$OUT/phase2_executor" -lvulkan

"$OUT/phase2_executor" \
  "$OUT/phase2.vert.spv" \
  "$OUT/phase2.frag.spv" \
  "$OUT/frame.rgba" \
  "$OUT/phase2-evidence.json"

ENCODER="h264_nvenc"
if ! ffmpeg -hide_banner -loglevel error -y \
    -f rawvideo -pix_fmt rgba -s 256x256 -r 60 -i "$OUT/frame.rgba" \
    -frames:v 1 -c:v h264_nvenc -preset p4 -tune ll -f h264 "$OUT/frame.h264"; then
  if [[ "${RGPU_REQUIRE_NVENC:-0}" == "1" ]]; then
    echo "PHASE2_COMPRESSED_FRAME=FAIL encoder=h264_nvenc" >&2
    exit 31
  fi
  ENCODER="libx264"
  ffmpeg -hide_banner -loglevel error -y \
    -f rawvideo -pix_fmt rgba -s 256x256 -r 60 -i "$OUT/frame.rgba" \
    -frames:v 1 -c:v libx264 -preset veryfast -tune zerolatency -f h264 "$OUT/frame.h264"
fi

RAW_SIZE="$(stat -c %s "$OUT/frame.rgba")"
H264_SIZE="$(stat -c %s "$OUT/frame.h264")"
[[ "$RAW_SIZE" -eq 262144 ]]
[[ "$H264_SIZE" -gt 0 ]]
CODEC="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$OUT/frame.h264")"
[[ "$CODEC" == "h264" ]]
SHA256="$(sha256sum "$OUT/frame.h264" | awk '{print $1}')"
GPU="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["gpu"])' "$OUT/phase2-evidence.json")"
HARDWARE_VULKAN="$(python3 -c 'import json,sys; print(str(json.load(open(sys.argv[1]))["hardware_vulkan"]).lower())' "$OUT/phase2-evidence.json")"
if [[ "${RGPU_REQUIRE_HARDWARE_VULKAN:-0}" == "1" && "$HARDWARE_VULKAN" != "true" ]]; then
  echo "PHASE2_HARDWARE_VULKAN=FAIL gpu=$GPU" >&2
  exit 32
fi

python3 - "$OUT/phase2-evidence.json" "$ENCODER" "$H264_SIZE" "$SHA256" <<'PY'
import json, sys
path, encoder, size, digest = sys.argv[1:]
data = json.load(open(path, encoding="utf-8"))
data.update({
    "compressed_frame_return": True,
    "compressed_codec": "h264",
    "compressed_encoder": encoder,
    "compressed_bytes": int(size),
    "compressed_sha256": digest,
})
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY

{
  echo "PHASE2_REMOTE_EXECUTOR=PASS"
  echo "GPU=$GPU"
  echo "HARDWARE_VULKAN=$HARDWARE_VULKAN"
  echo "RESOURCE_CREATION=PASS"
  echo "SHADER_AND_PSO_CREATION=PASS"
  echo "UPLOAD_HEAP=PASS"
  echo "DESCRIPTOR_RECONSTRUCTION=PASS"
  echo "COMMAND_LIST=PASS"
  echo "RENDERED_FRAME=PASS"
  echo "FENCE_COMPLETION=PASS"
  echo "COMPRESSED_FRAME_RETURN=PASS"
  echo "ENCODER=$ENCODER"
  echo "RAW_BYTES=$RAW_SIZE"
  echo "H264_BYTES=$H264_SIZE"
  echo "H264_SHA256=$SHA256"
} | tee "$OUT/phase2-summary.txt"

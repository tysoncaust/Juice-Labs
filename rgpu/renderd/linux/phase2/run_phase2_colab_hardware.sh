#!/usr/bin/env bash
set -euo pipefail

PHASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$PHASE/../../../.." && pwd)"
OUT="$PHASE/out-colab"
mkdir -p "$OUT"
rm -f "$OUT"/*

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential glslang-tools vulkan-tools libvulkan-dev ffmpeg python3

# Source this so the selected NVIDIA manifest remains in this process.
source "$REPO/rgpu/colab/install_nvidia_vulkan.sh"
export VK_DRIVER_FILES="${VK_ICD_FILENAMES:-${VK_DRIVER_FILES:-}}"
if [[ -z "${VK_DRIVER_FILES}" ]]; then
  VK_DRIVER_FILES="$(find /usr/share/vulkan/icd.d /etc/vulkan/icd.d -maxdepth 1 -type f -iname '*nvidia*.json' 2>/dev/null | head -n1)"
fi
[[ -f "$VK_DRIVER_FILES" ]]
export VK_LOADER_DRIVERS_SELECT='*nvidia*'
export VK_LOADER_LAYERS_DISABLE='*'
export RGPU_EXPECT_DEVICE='NVIDIA'
export RGPU_EXPECT_VENDOR_ID='0x10de'
export RGPU_TIMESTAMP_ITERATIONS="${RGPU_TIMESTAMP_ITERATIONS:-8192}"

glslangValidator -V "$PHASE/shaders/phase2.vert" -o "$OUT/phase2.vert.spv"
glslangValidator -V "$PHASE/shaders/phase2.frag" -o "$OUT/phase2.frag.spv"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  "$PHASE/phase2_executor.cpp" -o "$OUT/phase2_executor" -lvulkan
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  "$PHASE/phase2_hardware_probe.cpp" -o "$OUT/phase2_hardware_probe" -lvulkan

vulkaninfo --summary > "$OUT/vulkaninfo-summary.txt" 2> "$OUT/vulkaninfo-summary.err.txt"
nvidia-smi dmon -s u -d 1 -c 8 > "$OUT/nvidia-dmon.txt" 2> "$OUT/nvidia-dmon.err.txt" &
DMON_PID=$!
sleep 0.3

"$OUT/phase2_hardware_probe" "$OUT/hardware-probe.json"
"$OUT/phase2_executor" \
  "$OUT/phase2.vert.spv" "$OUT/phase2.frag.spv" \
  "$OUT/frame.rgba" "$OUT/render-evidence.json"
wait "$DMON_PID"

ffmpeg -hide_banner -loglevel error -y \
  -f rawvideo -pixel_format rgba -video_size 256x256 -framerate 60 \
  -i "$OUT/frame.rgba" -frames:v 1 \
  -c:v h264_nvenc -preset p4 -tune ll -f h264 "$OUT/frame.h264"
CODEC="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1:nk=1 "$OUT/frame.h264")"
[[ "$CODEC" == 'h264' ]]

python3 - "$REPO" "$OUT" "$VK_DRIVER_FILES" <<'PY'
from __future__ import annotations
import hashlib
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone

repo = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
icd_manifest = pathlib.Path(sys.argv[3]).resolve()

def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()

probe = json.loads((out / 'hardware-probe.json').read_text())
render = json.loads((out / 'render-evidence.json').read_text())
if not probe.get('hardware_vulkan') or probe.get('vendor_id') != 0x10DE:
    raise SystemExit('hardware NVIDIA Vulkan gate failed')
if re.search(r'llvmpipe|lavapipe|software|swiftshader|cpu', probe.get('device_name', ''), re.I):
    raise SystemExit('software Vulkan device selected')
if not all(render.get(k) for k in ('resource_creation','graphics_pipeline_created','fence_completed','frame_non_uniform')):
    raise SystemExit('renderer acceptance failed')

samples = []
for line in (out / 'nvidia-dmon.txt').read_text(errors='replace').splitlines():
    match = re.match(r'^\s*\d+\s+(\d+)\s+', line)
    if match:
        samples.append(int(match.group(1)))
max_sm = max(samples, default=0)
if max_sm <= 0:
    raise SystemExit('nvidia-smi did not observe SM utilization')

reference = json.loads((repo / 'rgpu/renderd/linux/phase2/out/phase2-evidence.json').read_text())
if render['frame_crc32'] != reference['frame_crc32']:
    raise SystemExit(f"frame mismatch {render['frame_crc32']} != {reference['frame_crc32']}")

icd = json.loads(icd_manifest.read_text())
library = pathlib.Path(icd['ICD']['library_path'])
if not library.is_absolute():
    library = (icd_manifest.parent / library).resolve()

manifest = {
    'acceptance': 'PHASE2_COLAB_HARDWARE_VULKAN_PASS',
    'collected_utc': datetime.now(timezone.utc).isoformat(),
    'git_commit': command('git','-C',str(repo),'rev-parse','HEAD'),
    'environment': {
        'operating_system': platform.platform(),
        'kernel': platform.release(),
        'machine': platform.machine(),
        'python': platform.python_version(),
        'vulkan_loader_summary_sha256': sha(out / 'vulkaninfo-summary.txt'),
        'selected_icd_manifest': str(icd_manifest),
        'selected_icd_manifest_sha256': sha(icd_manifest),
        'selected_icd_library': str(library),
        'selected_icd_library_sha256': sha(library),
        'driver_override': os.environ.get('VK_DRIVER_FILES'),
        'driver_filter': os.environ.get('VK_LOADER_DRIVERS_SELECT'),
        'nvidia_smi_identity': command('nvidia-smi','--query-gpu=name,pci.bus_id,driver_version,uuid','--format=csv,noheader,nounits').splitlines()[0],
        'nvidia_smi_full_sha256': hashlib.sha256(command('nvidia-smi','-q').encode()).hexdigest(),
    },
    'vulkan': probe,
    'renderer': render,
    'independent_gpu_activity': {
        'observer': 'nvidia-smi dmon',
        'max_sm_utilization_percent': max_sm,
        'samples': samples,
        'raw_log_sha256': sha(out / 'nvidia-dmon.txt'),
    },
    'output': {
        'raw_rgba_bytes': (out / 'frame.rgba').stat().st_size,
        'raw_rgba_sha256': sha(out / 'frame.rgba'),
        'frame_crc32': render['frame_crc32'],
        'reference_crc32': reference['frame_crc32'],
        'reference_frame_match': True,
        'compressed_codec': 'h264',
        'compressed_encoder': 'h264_nvenc',
        'compressed_bytes': (out / 'frame.h264').stat().st_size,
        'compressed_sha256': sha(out / 'frame.h264'),
    },
}
(out / 'phase2-colab-hardware-evidence.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
summary = [
    'PHASE2_COLAB_HARDWARE_VULKAN=PASS',
    f"DEVICE={probe['device_name']}",
    f"DRIVER={probe['driver_name']} {probe['driver_info']}",
    f"ICD_MANIFEST={icd_manifest}",
    f"GPU_TIMESTAMP_TICKS={probe['timestamp_delta_ticks']}",
    f"GPU_TIMESTAMP_NS={probe['timestamp_elapsed_ns']}",
    f"INDEPENDENT_MAX_SM_UTILIZATION_PERCENT={max_sm}",
    f"FRAME_CRC32={render['frame_crc32']}",
    'REFERENCE_FRAME_MATCH=PASS',
    'NVENC_H264_RETURN=PASS',
]
(out / 'phase2-colab-summary.txt').write_text('\n'.join(summary) + '\n')
print('\n'.join(summary))
PY

if [[ -n "${RGPU_DRIVE_EVIDENCE_DIR:-}" ]]; then
  mkdir -p "$RGPU_DRIVE_EVIDENCE_DIR"
  cp -f "$OUT/phase2-colab-hardware-evidence.json" "$RGPU_DRIVE_EVIDENCE_DIR/"
  cp -f "$OUT/phase2-colab-summary.txt" "$RGPU_DRIVE_EVIDENCE_DIR/"
  cp -f "$OUT/vulkaninfo-summary.txt" "$RGPU_DRIVE_EVIDENCE_DIR/"
  cp -f "$OUT/nvidia-dmon.txt" "$RGPU_DRIVE_EVIDENCE_DIR/"
fi

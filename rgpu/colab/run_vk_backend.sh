#!/usr/bin/env bash
# rgpu Linux Vulkan + H.264 backend — build + verify on a Colab NVIDIA GPU.
# Installs the matching NVIDIA graphics userspace so Vulkan uses the T4 (not
# llvmpipe), renders the rgpu protocol offscreen, runs a GPU benchmark, and
# H.264-encodes via the T4 NVENC.  Run from the repo root on a GPU runtime:
#   !bash rgpu/colab/run_vk_backend.sh
set -e
echo "===== 0. GPU ====="
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader || { echo "NO GPU RUNTIME"; exit 1; }

echo "===== 1. NVIDIA Vulkan userspace (matching driver branch) + deps ====="
DEBIAN_FRONTEND=noninteractive bash rgpu/colab/install_nvidia_vulkan.sh || echo "(hardware Vulkan userspace install failed; will fall back to software)"
apt-get -qq install -y build-essential ffmpeg >/dev/null 2>&1 || true

# Prefer the NVIDIA ICD explicitly if it was installed (safer than removing mesa).
ICD="$(find /usr/share/vulkan/icd.d /etc/vulkan/icd.d -maxdepth 1 -type f -iname '*nvidia*.json' 2>/dev/null | head -n1)"
[ -n "$ICD" ] && export VK_ICD_FILENAMES="$ICD" && echo "using NVIDIA ICD: $ICD"

echo "===== 2. build the rgpu Vulkan renderer ====="
g++ -std=c++17 -O2 rgpu/renderd/linux/rgpu_renderd_vk.cpp -o /content/rgpu_renderd_vk -lvulkan
echo "built /content/rgpu_renderd_vk"

echo "===== 3. loopback: render an rgpu protocol batch (must match CLEAR_RTV color) ====="
python3 rgpu/renderd/linux/gen_batch.py /content/batch.bin
/content/rgpu_renderd_vk /content/batch.bin /content/frame.raw
RC=$?

echo "===== 4. GPU benchmark: 3600 offscreen frames @ 1280x720 (Vulkan timestamps) ====="
/content/rgpu_renderd_vk --headless --width 1280 --height 720 --frames 3600 --benchmark-json /content/benchmark.json || true
echo "--- benchmark.json ---"; cat /content/benchmark.json 2>/dev/null

echo "===== 5. H.264 encode the rendered frame(s) on the T4 NVENC ====="
python3 - <<'PY'
data=open('/content/frame.raw','rb').read()
open('/content/clip.raw','wb').write(data*60)   # 60 frames
print('clip.raw', len(data*60), 'bytes (60 frames)')
PY
ENC=libx264
# NVENC has a minimum frame size; scale the 64x64 target up for that path.
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -vf scale=256:256 -c:v h264_nvenc -pix_fmt yuv420p /content/out.mp4 2>/dev/null && ENC="h264_nvenc (T4 hardware)" || \
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -c:v libx264 -pix_fmt yuv420p /content/out.mp4
echo "encoded with: $ENC"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name,width,height,nb_frames -of default=noprint_wrappers=1 /content/out.mp4
ls -l /content/out.mp4

echo "===== DONE (loopback exit=$RC) ====="
exit $RC

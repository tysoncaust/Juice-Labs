#!/usr/bin/env bash
# rgpu Linux Vulkan + H.264 backend — build + verify on a Colab NVIDIA GPU.
# Run from the repo root on Colab (after git clone), on a GPU runtime:
#   !bash rgpu/colab/run_vk_backend.sh
set -e
echo "===== 0. GPU ====="
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader || { echo "NO GPU RUNTIME"; exit 1; }

echo "===== 1. deps (Vulkan loader + tools + build + ffmpeg) ====="
apt-get -qq update >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get -qq install -y \
  libvulkan1 libvulkan-dev vulkan-tools build-essential ffmpeg >/dev/null 2>&1
echo "installed."

echo "===== 2. register the NVIDIA Vulkan ICD (if this host's driver exposes it) ====="
# On a host with the FULL NVIDIA driver, libGLX_nvidia.so.0 exports the Vulkan ICD
# entry (vk_icdGetInstanceProcAddr) and this makes the renderer use the GPU. NOTE:
# free Colab GPU runtimes ship a CUDA-COMPUTE driver whose libGLX_nvidia.so.0 does
# NOT export that symbol (vkCreateInstance -> ERROR_INCOMPATIBLE_DRIVER), so Vulkan
# there falls back to Mesa llvmpipe (software). We register the ICD anyway and let
# the loader pick the best device; the renderer prefers a discrete GPU when present.
mkdir -p /usr/share/vulkan/icd.d
GLX=$(find /usr/lib64-nvidia /usr/lib -name 'libGLX_nvidia.so.0' 2>/dev/null | head -1)
if [ -n "$GLX" ]; then
  echo /usr/lib64-nvidia > /etc/ld.so.conf.d/nvidia-gl.conf 2>/dev/null; ldconfig 2>/dev/null || true
  printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s","api_version":"1.3.0"}}\n' "$GLX" \
    > /usr/share/vulkan/icd.d/nvidia_icd.json
  echo "registered nvidia_icd.json -> $GLX"
  if ! VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json vulkaninfo --summary >/dev/null 2>&1; then
    echo "WARN: NVIDIA Vulkan ICD non-functional on this host (compute-only driver); using software Vulkan"
  fi
fi
echo "--- vulkaninfo device summary ---"
vulkaninfo --summary 2>/dev/null | grep -iE 'deviceName|deviceType|driverName' | head -12 || echo "(vulkaninfo unavailable)"

echo "===== 3. build the rgpu Vulkan renderer ====="
g++ -std=c++17 -O2 rgpu/renderd/linux/rgpu_renderd_vk.cpp -o /content/rgpu_renderd_vk -lvulkan
echo "built /content/rgpu_renderd_vk"

echo "===== 4. generate an rgpu protocol batch + render it on the GPU ====="
python3 rgpu/renderd/linux/gen_batch.py /content/batch.bin
/content/rgpu_renderd_vk /content/batch.bin /content/frame.raw
RC=$?

echo "===== 5. H.264 encode the rendered frame(s) ====="
# replicate the frame into a 2s clip and encode to H.264 (NVENC if available, else libx264)
python3 - <<'PY'
data=open('/content/frame.raw','rb').read()
open('/content/clip.raw','wb').write(data*60)   # 60 frames
print('clip.raw', len(data*60), 'bytes (60 frames)')
PY
ENC=libx264
# Prefer the GPU's NVENC engine (uses the CUDA/compute driver, works on Colab even
# though Vulkan does not). NVENC has a minimum frame size, so scale the 64x64 test
# target up to 256x256 for the NVENC path; fall back to CPU libx264 at native size.
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -vf scale=256:256 -c:v h264_nvenc -pix_fmt yuv420p /content/out.mp4 2>/dev/null && ENC="h264_nvenc (T4 hardware)" || \
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -c:v libx264 -pix_fmt yuv420p /content/out.mp4
echo "encoded with: $ENC"
echo "--- ffprobe out.mp4 ---"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name,width,height,nb_frames -of default=noprint_wrappers=1 /content/out.mp4
ls -l /content/out.mp4

echo "===== DONE (renderer exit=$RC) ====="
exit $RC

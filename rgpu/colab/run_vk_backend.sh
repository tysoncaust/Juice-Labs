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

echo "===== 2. ensure NVIDIA Vulkan ICD is registered ====="
mkdir -p /usr/share/vulkan/icd.d
if ! vulkaninfo --summary >/dev/null 2>&1; then
  GLX=$(ldconfig -p 2>/dev/null | grep -m1 'libGLX_nvidia.so.0' | awk '{print $NF}')
  [ -z "$GLX" ] && GLX=$(find / -name 'libGLX_nvidia.so.0' 2>/dev/null | head -1)
  if [ -n "$GLX" ]; then
    printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s","api_version":"1.3.0"}}\n' "$GLX" \
      > /usr/share/vulkan/icd.d/nvidia_icd.json
    echo "wrote nvidia_icd.json -> $GLX"
  else
    echo "libGLX_nvidia.so.0 not found; Vulkan may fall back to software (llvmpipe)"
  fi
fi
echo "--- vulkaninfo device summary ---"
vulkaninfo --summary 2>/dev/null | grep -A3 -iE 'deviceName|deviceType' | head -20 || echo "(vulkaninfo unavailable)"

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
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -c:v h264_nvenc -pix_fmt yuv420p /content/out.mp4 2>/dev/null && ENC=h264_nvenc || \
ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s 64x64 -r 30 -i /content/clip.raw \
  -c:v libx264 -pix_fmt yuv420p /content/out.mp4
echo "encoded with: $ENC"
echo "--- ffprobe out.mp4 ---"
ffprobe -hide_banner -loglevel error -show_entries stream=codec_name,width,height,nb_frames -of default=noprint_wrappers=1 /content/out.mp4
ls -l /content/out.mp4

echo "===== DONE (renderer exit=$RC) ====="
exit $RC

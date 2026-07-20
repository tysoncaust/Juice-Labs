#!/usr/bin/env bash
# Install the FULL NVIDIA graphics userspace matching the loaded driver branch so
# the Colab GPU exposes a real hardware Vulkan device (not Mesa llvmpipe). The
# Vulkan loader gets vkCreateInstance + global entry points from the vendor ICD;
# installing only vulkan-tools or pointing at an incomplete libGLX_nvidia.so.0 is
# NOT enough — the matching libnvidia-gl-<branch> package is required.
#   Verify success: vulkaninfo --summary must show deviceName=Tesla T4 /
#   deviceType=PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, NOT llvmpipe/lavapipe/CPU.
set -euo pipefail

apt-get update -qq
apt-get install -y vulkan-tools

driver_version="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -n1)"
driver_major="${driver_version%%.*}"
echo "Loaded NVIDIA driver: ${driver_version}"
echo "Looking for graphics package for branch: ${driver_major}"

package=""
if apt-cache show "libnvidia-gl-${driver_major}" >/dev/null 2>&1; then
    package="libnvidia-gl-${driver_major}"
elif apt-cache show "libnvidia-gl-${driver_major}-server" >/dev/null 2>&1; then
    package="libnvidia-gl-${driver_major}-server"
else
    echo "No matching libnvidia-gl package for branch ${driver_major}."
    echo "Available libnvidia-gl packages:"
    apt-cache search '^libnvidia-gl-[0-9]+(-server)?$' || true
    exit 2
fi

echo "Installing ${package}"
apt-get install -y "${package}"

icd="$(find /usr/share/vulkan/icd.d /etc/vulkan/icd.d -maxdepth 1 -type f -iname '*nvidia*.json' 2>/dev/null | head -n1)"
if [ -z "${icd}" ]; then
    echo "NVIDIA Vulkan ICD manifest was not installed."
    exit 3
fi
echo "Using Vulkan ICD: ${icd}"

# Select the NVIDIA manifest explicitly (safer than removing mesa-vulkan-drivers).
export VK_ICD_FILENAMES="${icd}"
vulkaninfo --summary

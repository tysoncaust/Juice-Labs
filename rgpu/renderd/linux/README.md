# rgpu-renderd-linux — Vulkan backend (the remote/Colab renderer)

The Linux renderer that consumes the rgpu protocol on a GPU, for the remote leg
(Colab / cloud). It is the Vulkan counterpart of the Windows D3D11 reference
renderer: the protocol expresses D3D11 *semantics*, and this backend implements
them independently in Vulkan — NOT a forwarding of Windows calls.

## Files
- `rgpu_renderd_vk.cpp` — headless Vulkan renderer. Parses an rgpu protocol
  `COMMAND_BATCH` (`CREATE_TEXTURE_2D` → offscreen color target, `CLEAR_RTV` →
  render-pass load-op clear to the batch color, `PRESENT` → readback), copies the
  image to a host-visible buffer, verifies the pixel == the requested color (the
  same check as the Windows D3D11 loopback), and writes the RGBA frame to disk.
- `gen_batch.py` — emits the protocol batch (cross-language wire-compat proof).
- `../../colab/run_vk_backend.sh` — builds + runs on a Colab GPU and H.264-encodes.

## Verified on the real Colab NVIDIA Tesla T4 (hardware Vulkan)
Run `rgpu/colab/install_nvidia_vulkan.sh` first — it installs
`libnvidia-gl-<driver_major>` (the vendor Vulkan ICD userspace matching the loaded
driver branch). After that, `vulkaninfo --summary` enumerates the T4 as a discrete
device (`deviceName=Tesla T4`, `driverName=NVIDIA`, `PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`).
Then `run_vk_backend.sh` verified all three legs **on the T4**:
- **Protocol → Vulkan render → readback: PASS on the GPU.** Renderer reported
  `GPU: Tesla T4 … HARDWARE GPU`, `software_renderer=false`; cleared the target to
  `(0.2,0.4,0.8,1.0)` and read back the exact pixel — the rgpu protocol renders on
  the real hardware.
- **Offscreen benchmark: PASS on the GPU.** 3600 frames @ 1280×720, measured with
  Vulkan timestamp queries, `benchmark.json` records `software_renderer:false`,
  `gpu:"Tesla T4"`. (The per-frame workload is a load-op clear, so `average_gpu_ms`
  is ~0 and the FPS figure reflects pipeline/timestamp overhead, not a real scene —
  add geometry/shaders for a representative number.)
- **H.264 encode: PASS on the T4 NVENC hardware.** `h264_nvenc` (`ffprobe`:
  `codec=h264`). NVENC needs ≥ a minimum frame size, so the small test target is
  scaled up for that path.

## Correction (earlier claim was wrong)
An earlier note here said free Colab GPUs "can't do Vulkan" — that was an overreach.
The initial `VK_ERROR_INCOMPATIBLE_DRIVER` came from a **missing matching NVIDIA
graphics userspace**, not a hard limitation: Colab exposes the kernel/compute driver
but not the full graphics libs by default, and `libGLX_nvidia.so.0` alone doesn't
export the Vulkan ICD entry point. Installing `libnvidia-gl-<branch>` (the vendor ICD)
fixes it and the T4 does hardware Vulkan.

## Offscreen only (no swapchain) — the right shape for headless GPUs
The renderer is fully offscreen: it renders into ordinary device-local `VkImage`
targets and copies/encodes them — no `VkSurfaceKHR`, no `VkSwapchainKHR`, no
`vkQueuePresentKHR`, no X11/Wayland. That is what lets it run on a display-less
Colab GPU. For live streaming, rotate 2–3 render images (frame N rendering, N-1
NVENC-encoding, N-2 transmitting) and convert RGBA→NV12 for NVENC.

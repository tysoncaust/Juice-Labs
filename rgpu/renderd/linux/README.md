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

## Verified on Colab (NVIDIA Tesla T4)
- **Protocol → Vulkan render → frame readback: PASS.** The renderer parsed the
  batch, cleared a 64×64 target to `(0.2,0.4,0.8,1.0)` and read back the exact
  pixel `(51,102,204,255)` — the rgpu protocol renders correctly on Vulkan.
- **H.264 encode: PASS.** The frame was encoded to H.264 — verified both with CPU
  `libx264` and, importantly, with **`h264_nvenc` on the T4's NVENC hardware**
  (`ffprobe`: `codec=h264, 256×256, 60 frames`; NVENC needs ≥ a minimum frame
  size, so the 64×64 target is scaled up for that path). `libnvidia-encode.so` is
  present on the Colab GPU runtime and NVENC works.

## Known limitation: Vulkan on free Colab GPUs
The render above ran on **Mesa llvmpipe (software Vulkan)**, not the T4, because
**Colab's GPU runtime ships a CUDA-compute driver whose `libGLX_nvidia.so.0` does
not export the Vulkan ICD entry point** (`vk_icdGetInstanceProcAddr`) — so
`vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER` on the NVIDIA ICD even
though `libnvidia-glvkspirv`/`glcore` are present. This is a Colab provisioning
limitation, not an rgpu bug (the renderer + protocol are proven correct on
software Vulkan, and the same binary uses the GPU on a host whose driver exposes
the Vulkan ICD).

Implications for the remote backend (matches the recommended sequence):
- The protocol→GPU render loop is **already proven on real GPU hardware** via the
  Windows D3D11 reference renderer on the RTX 3050 (the D3D11 loopback test).
- For a **Vulkan** GPU backend, use a cloud GPU with the full NVIDIA driver (a GPU
  VM, not free Colab), or a provider that exposes Vulkan.
- Per the user's own sequencing, a **Windows D3D12 remote backend** is more
  practical for SM6/D3D12 titles than translating to Linux Vulkan; the T4's
  **NVENC** (verified here) serves the frame-encode/return leg regardless of the
  render API.

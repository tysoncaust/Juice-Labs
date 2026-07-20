# rgpu/colab — Colab GPU backend + ComfyUI model proofs

## Capability probe (`rgpu_probe.py`, `rgpu_vk_probe.c`)
Runs at Colab startup: verifies the runtime can genuinely render headlessly
(nvidia-smi + Vulkan/EGL/CUDA + `vulkaninfo` + a native `vkCreateDevice` test)
and emits the structured capability report the controller uses to accept/reject
an allocation. `python3 rgpu_probe.py --self-test` passes locally; on Colab run
`python3 rgpu_probe.py --strict` (exit 2 if not allocatable). Build the native
test with `cc rgpu_vk_probe.c -o rgpu_vk_probe -lvulkan` (after `apt-get install
libvulkan-dev`).

## ComfyUI full-quality proofs (`comfyui_models_proof.ipynb` / `.py`)
Open the notebook in Colab on a **GPU** runtime. It installs ComfyUI + ComfyUI-
Manager, downloads the models, generates a **full-quality SDXL** image headlessly
(verified graph: 1024×1024, 30 steps, dpmpp_2m/karras), and opens a cloudflared
tunnel to the ComfyUI UI so **Chroma1-HD** (Flux-8.9B) and **Z-Image** can be run
interactively with the downloaded checkpoints.

Models (edit `MODELS` in the `.py` if an upstream repo/file changed):
- **SDXL** — `stabilityai/stable-diffusion-xl-base-1.0` → `models/checkpoints`.
- **Chroma1-HD** — `lodestones/Chroma1-HD` (+ flux t5xxl/clip_l text encoders + FLUX `ae.safetensors` VAE).
- **Z-Image** — `Tongyi-MAI/Z-Image-Turbo` (needs a current ComfyUI with Z-Image nodes).

### Why Colab and not the dev box
Full-quality Chroma1-HD and Z-Image need a real ≥15 GB GPU (Colab T4/L4). The RTX
3050 (4 GB) can't run them — this is the on-demand Colab path the project targets.
The SDXL cell is fully automated; the two newer models' exact node graphs vary by
ComfyUI version, so they're wired for interactive runs over the tunnel (with the
models pre-downloaded) rather than hard-coded headless graphs that might drift.
Proof images save to `/content/rgpu_proofs` and can be copied to Drive.

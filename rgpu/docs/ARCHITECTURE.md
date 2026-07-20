# rgpu — portable GPU-over-IP graphics data plane

Goal: a **CPU/RAM-only** machine runs a Windows game; a **remote, on-demand Colab
GPU** does the rendering; frames come back to a virtual display. The open-source
Juice fork gives us orchestration (agent/controller/juicify); the closed-source
interception engine (`juiceclient.dll`) does not exist for us, so `rgpu` is the
**own** graphics data plane that replaces it.

```
Windows game
  → D3D11 interception/proxy      (rgpu-proxy-d3d11, this repo)
  → versioned remote graphics protocol  (rgpu/proto)
  → renderer                      (rgpu-renderd: Windows/D3D11 reference, Linux/Vulkan for Colab)
  → NVIDIA GPU (RTX locally for dev, Colab T4 for prod)
  → encoded frames back → virtual display on the client
```

This is **not** function-pointer forwarding. The protocol expresses D3D11
*semantics* so a Linux Vulkan renderer can implement them independently.

## Hard policies (fail closed)
- `remote_required = true`, `local_hardware_fallback = false`, `warp_game_fallback = false`.
- The proxy NEVER calls the system `D3D11CreateDevice` on real hardware. If the
  remote GPU is lost: stop rendering, surface "Remote GPU session lost", do **not**
  silently fall back to a local adapter.
- Debug builds **abort** if any unproxied hardware device is created.
- Success criterion at the enumeration layer: `localHardwareDevicesCreated = 0`,
  `remoteDevicesCreated = 1`.

## The synthetic adapter
The game must see a virtual adapter representing the *active remote GPU*, e.g.
`Remote GPU - NVIDIA T4`, 15360 MB, D3D11 feature level 11_0/11_1, "Remote session".
Proxied entrypoints report **negotiated remote capabilities**, never local hardware:
`CreateDXGIFactory*`, `IDXGIFactory::EnumAdapters*`, `IDXGIAdapter::GetDesc*`,
`D3D11CreateDevice*`, `D3D11CreateDeviceAndSwapChain*`. Only capabilities the
remote renderer genuinely supports are advertised (VRAM, feature level, shader
model, formats, MSAA, compute), because games pre-flight these.

## Colab reality
Colab runtimes are Linux (Ubuntu 22.04). They can't run Windows D3D11. So the
Colab backend is a **Linux Vulkan renderer** that implements the protocol. Every
Colab boot runs a capability probe (nvidia-smi + Vulkan + encoders + a real
headless device-create test) and reports structured capabilities to the
controller; an allocation is rejected if Vulkan or the required encoder is absent.

## Transport
One authenticated WebSocket over TLS for the prototype, carrying: control,
resource uploads, ordered command batches, synchronous query responses, encoded
frame chunks, heartbeats, reconnect, cancel. The **renderer dials out** to a
stable rendezvous/controller (the Colab address is ephemeral and must not be
baked into the client). Later: split into multiple transport streams.

## Reuse from the existing bridge
Keep the lifecycle concepts from `local_runtime/tools/gpu_bridge.py`: register /
status / release, loopback endpoint presentation, dynamically-changing remote
destination. But graphics needs a **binary** protocol — an HTTP forwarder built
for model APIs is not enough for texture uploads / ordered batches / frames.

## Testing without a second physical machine
Dev on the RTX host, but validate the *real property* with a **GPU-less Windows
VM** (Hyper-V, no GPU passthrough/partition, synthetic display only) that connects
to a renderer running on the host. Verify inside the VM: dxdiag shows only a
synthetic adapter, Device Manager has no RTX 3050, client log shows
`localHardwareDevicesCreated=0 / remoteDevicesCreated=1`. On the host, RTX activity
belongs to `rgpu-renderd`, not the game. Pull the virtual network → the game must
STOP rendering, not fall back locally.

## Phased roadmap (maps to the spec)
- **Phase 1** — Simulated GPU-less client → custom D3D11 proxy → **RTX host D3D11
  reference renderer**. Validates synthetic-adapter enumeration, injection,
  no-local-fallback, protocol, resource mgmt, frame return, game startup.
- **Phase 2** — Portable renderer: `rgpu-renderd-win` (D3D11 reference) and
  `rgpu-renderd-linux` (Vulkan) consuming the same protocol.
- **Phase 3** — Colab remote triangle: GPU-less Windows VM → internet → Colab
  Vulkan renderer → frame returned.
- **Phase 4** — API coverage: textures, dynamic VBs, depth/stencil, mipmaps,
  shader compile, compute, copies, Map/Unmap, queries, resize, fullscreen,
  device-loss.
- **Phase 5** — Games (Subnautica, TCG Card Shop Simulator, Tokyo Xtreme Racer).
  Game gate = launched on GPU-less VM, remote device accepted, **first frame
  rendered by the remote GPU**, menu reached, **no local GPU device created**.

## Status (2026-07-20)
- [x] Project skeleton + this architecture doc.
- [x] Colab capability probe + structured report (`rgpu/colab/`) — self-test passes,
      emits the exact `provider/gpu/graphics/encoding/d3dFrontend` schema, fail-closed
      allocation decision. Includes the native headless `vkCreateDevice` test.
- [x] Versioned protocol definition (`rgpu/proto/`) — compiles.
- [x] Phase-1 proof: D3D11/DXGI **synthetic-adapter enumeration + fail-closed
      policy** (`rgpu/proxy-d3d11/`) — **compiles with mingw and all checks pass**:
      the game enumerates only "Remote GPU - NVIDIA T4" (0x10DE, 15360 MB);
      `localHardwareDevicesCreated=0`, `remoteDevicesCreated=1`, WARP refused. This
      is the interception *surface + policy*, NOT yet a command-serializing renderer.
- [x] Host D3D11 **reference renderer** (`rgpu/renderd/win`) — consumes a protocol
      batch on a real device (create RT → clear → present → readback); loopback
      test verifies presented pixels == cleared color. + stdio CLI.
- [x] **WebSocket transport + ephemeral registration** (`rgpu/controller`, `rgpu/renderd/rgpu_agent.py`)
      — agent dials out, registers caps, client opens a session; a command batch
      flows client→controller→agent→native renderer and a real frame returns.
      **Verified networked frame** (pixels correct).
- [x] **dxgi proxy DLL** (`proxy-d3d11/src/rgpu_dxgi_proxy.cpp`) — injectable
      `dxgi.dll` routing `CreateDXGIFactory*` to the synthetic adapter, forwarding
      the rest to the real dxgi. Verified: a loader gets the synthetic adapter.
- [x] **Client serialization core** (`proxy-d3d11/src/rgpu_serializer.*`) — a
      device/context-shaped front-end whose D3D11-subset calls serialize into the
      protocol. Verified: game-shaped calls → protocol → renderer → correct frame.
- [x] **Colab ComfyUI proof runner** (`rgpu/colab/comfyui_models_proof.*`) — SDXL
      headless (verified graph) + Chroma1-HD/Z-Image over a ComfyUI tunnel; runs on
      the on-demand Colab GPU.
- [x] **Full `ID3D11Device` COM vtable (40 methods) + `d3d11.dll` proxy** — pass-through
      +tee (`proxy-d3d11/src/rgpu_d3d11_device.h`, `rgpu_d3d11_proxy.cpp`). The proxy's
      exported `D3D11CreateDevice`/`...AndSwapChain` build a real device and hand the game
      an `RgpuD3D11Device` wrapper; covered creates tee into the protocol. Harness
      (`test/rgpu_device_harness.cpp`) verified on the RTX: device via the proxy export,
      real frame rendered through the wrapper (pixel exact), tee'd to a protocol batch.
- [x] **Real-game boot tests (Phase-5 device gate, local reference renderer)** — proxy
      `d3d11.dll` dropped beside each game; `%TEMP%\rgpu_proxy.log` records attach+wrap:
      * **Subnautica** (Unity/D3D11): booted to the "Subnautica" menu window, 2 devices
        wrapped, **GPU 3D-engine 100%, 1314 MB VRAM** — runs through our layer, GPU not refused.
      * **TCG Card Shop Simulator** (Unity/D3D11): booted to its window, 2 devices wrapped,
        **GPU 3D-engine 96%, 644 MB VRAM** — runs through our layer, GPU not refused.
      * **Tokyo Xtreme Racer** (UE5): forced `-d3d11` → proxy created **7 devices, no refusal**,
        but the game exits in renderer init because it is a **D3D12/SM6 title** (ships
        `D3D12`+`DML` RHIs; Nanite/Lumen can't run on D3D11). Default D3D12 launch boots
        fine (control). Intercepting it needs an `ID3D12Device` proxy, not a D3D11 one.
      No incompatible-driver errors, session kicks, or altered behaviour observed on the
      two D3D11 titles; both are single-player with no kernel anti-cheat.

- [x] **Full `ID3D11DeviceContext` COM vtable (115 entries)** — `rgpu_d3d11_context.h`
      wraps the real immediate context; every method forwards (game renders locally)
      and the per-frame Draw/DrawIndexed/DrawInstanced/Dispatch/Clear*/OMSetRenderTargets/
      CopyResource calls tee into the protocol. Device `GetImmediateContext` + the
      context returned by `D3D11CreateDevice` both hand back the one wrapped instance
      (COM identity). Harness verified: real frame + `contextsWrapped=1`,
      `tee{clears,state}` recorded. Bound resources are tee'd by a pointer-derived
      object id; full COM-pointer->id object-graph translation is the resource layer below.

- [x] **`rgpu-d3d12` frontend — step 1, transparent pass-through** (`rgpu/proxy-d3d12/`).
      A `d3d12.dll` proxy forwards the loader surface to `System32\d3d12.dll` (Agility
      SDK preserved) and logs device creation. **Verified: Tokyo Xtreme Racer boots
      unchanged on native D3D12 through it** — ATTACH + 4× `D3D12CreateDevice` (FL 12_0)
      S_OK, responsive rendering (GPU 3D ~93%, ~2.7 GB VRAM). Stops forcing `-d3d11`.

- [x] **Linux Vulkan backend + H.264 encode** (`rgpu/renderd/linux/`, `rgpu/colab/run_vk_backend.sh`).
      `rgpu_renderd_vk.cpp` consumes the rgpu protocol on Vulkan (CREATE_TEXTURE_2D/
      CLEAR_RTV/PRESENT) — the Vulkan counterpart of the D3D11 reference renderer.
      **Verified on a Colab T4:** protocol → render → readback returns the exact
      cleared pixel; the frame H.264-encodes both via libx264 AND via **h264_nvenc on
      the T4's NVENC hardware** (h264, 256×256, 60 frames). NOTE: the *render* ran on
      Mesa llvmpipe (software Vulkan) because Colab's compute-only driver doesn't
      export the NVIDIA Vulkan ICD entry (`vkCreateInstance` → INCOMPATIBLE_DRIVER) —
      a Colab limitation, not an rgpu bug. Real-GPU render is already proven via the
      Windows D3D11 reference renderer on the RTX; a Vulkan GPU backend needs a cloud
      GPU VM with the full NVIDIA driver. See `rgpu/renderd/linux/README.md`.

REMAINING (the multi-month graphics body):
- [ ] Resource object-graph translation for D3D11 (COM pointer -> stable id + upload
      path) so a REMOTE renderer can resolve the tee'd draws/state, not just count them.
- [ ] `rgpu-d3d12` step 2+: wrap the execution objects (device/queue/command-list/
      resource/heap/fence/PSO/root-sig + DXGI swap chain, identity-preserving), translate
      D3D12 handles, serialize at `ExecuteCommandLists`, DRED diagnostics. See
      `rgpu/proxy-d3d12/README.md`.
- [ ] Remote backend, in the user's sequence: D3D12 tee -> local replay -> remote
      **Windows D3D12** -> Linux **Vulkan + H.264** LAST (needs a Colab Linux GPU to verify).
- [ ] `rgpu-renderd-linux` (Vulkan) implementing the protocol on Colab + H.264
      frame encode + return to a virtual display (Phase-3 remote triangle → games).

The data-plane loop is proven end-to-end for a minimal frame (client D3D11 calls →
serialize → transport → remote renderer → verified frame back). Running a real game
needs the full COM surface + the Vulkan backend + encode — the remaining body.

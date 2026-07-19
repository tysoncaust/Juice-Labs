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
- [ ] Phase-1 host D3D11 reference renderer (`rgpu/renderd/win`).
- [ ] Command/resource serialization end-to-end (triangle).
- [ ] Phases 2–5.

This is the correct foundation and the first compilable proof; the remaining work
is the multi-month graphics-engineering body (protocol impl + Vulkan renderer).

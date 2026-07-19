# rgpu-proxy-d3d11 — synthetic adapter + fail-closed interception (Phase 1)

Presents the remote GPU to a Windows game as a **synthetic DXGI adapter** and
enforces the **fail-closed** policy: the game must never see local hardware and
must never fall back to a local/WARP device.

## What is proven here (compilable + tested)
`build.ps1` compiles `src/rgpu_synthetic.cpp` + `test/rgpu_enum_test.cpp` and runs
the harness. Verified output on the RTX host:

```
adapter: "Remote GPU - NVIDIA T4" vendor=0x10DE vram=15360 MB
VERIFICATION: localHardwareDevicesCreated=0  remoteDevicesCreated=1  refused=1
RESULT: all checks passed
```

- **Synthetic enumeration** — a real `IDXGIFactory1` whose `EnumAdapters`/
  `EnumAdapters1` return exactly one adapter, an `IDXGIAdapter1` whose
  `GetDesc`/`GetDesc1` report the negotiated remote caps (vendor, VRAM, feature
  level). This is what a game's pre-launch checks read.
- **Fail closed** — `rgpu_D3D11CreateDevice` never calls the system
  `D3D11CreateDevice` on hardware; with no remote session it returns
  `DXGI_ERROR_DEVICE_REMOVED` (no local fallback); `CreateSoftwareAdapter` (WARP)
  is refused; debug builds `abort()` via `rgpu_guard_local_hardware`.
- **Invariant** — `g_local_hw_devices_created` stays `0` always; a separate
  counter records refused attempts.

Caps come from `rgpu_default_caps()` and are meant to be overwritten by
`rgpu_set_caps()` from the protocol's `CAPS_OFFER` (`rgpu/proto`).

## What this is NOT (remaining work — the large body)
This is the **interception surface + policy**, not a working renderer. To run a
real game end-to-end still needs:
1. **Injection**: ship these entrypoints as a `dxgi.dll`/`d3d11.dll` proxy loaded
   ahead of the system ones (DLL search-order / hook), so a real game's calls
   land here. (This test links them directly.)
2. **A remote-backed `ID3D11Device`/`ID3D11DeviceContext`** whose methods
   serialize into the `rgpu/proto` command stream instead of executing locally.
3. **The renderer** (`rgpu/renderd`): a host D3D11 reference first, then the Linux
   Vulkan backend for Colab, plus frame encode + return to a virtual display.
4. **Transport** (`rgpu/proto` over WebSocket/TLS) + ephemeral agent registration.

See `../docs/ARCHITECTURE.md` for the full phased plan.

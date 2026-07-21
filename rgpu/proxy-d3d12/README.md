# rgpu-d3d12 — D3D12 interception frontend

D3D12/SM6 titles (e.g. Tokyo Xtreme Racer, UE5 with Nanite/Lumen) cannot render
on D3D11, so the D3D11 proxy can't intercept them. This is the parallel D3D12
frontend. It follows the incremental sequence: **transparent pass-through first,
serialize + remote later.**

## Status

- [x] **Step 1 — transparent pass-through proxy** (`src/rgpu_d3d12_proxy.cpp`).
      A `d3d12.dll` next to the game exports the loader surface a D3D12 game imports
      (`D3D12CreateDevice`, `D3D12GetInterface`, `D3D12GetDebugInterface`,
      `D3D12Serialize*RootSignature`, `D3D12Create*RootSignatureDeserializer`,
      `D3D12EnableExperimentalFeatures`) and forwards each to `System32\d3d12.dll`.
      Because the forward goes to the system d3d12.dll, the Agility SDK loader still
      reads the EXE's `D3D12SDKVersion`/`D3D12SDKPath` and boots the game's own
      `D3D12\...\D3D12Core.dll`. **Verified:** Tokyo Xtreme Racer boots unchanged.

- [x] **Full D3D12 COM wrapper object-graph** (`src/rgpu_d3d12_wrappers.h`,
      generated forwarders `src/rgpu_d3d12_wrappers_gen.h` from
      `tools/gen_wrappers.py`). The deterministic interceptor: `D3D12CreateDevice` (and
      the Agility `ID3D12DeviceFactory::CreateDevice`) return an **`RgpuD3D12Device`**
      wrapper instead of the raw device, so the holder cannot route around us.
      - **Identity-preserving `QueryInterface`**: returns `this` for every supported
        device revision (`ID3D12Device` .. `Device10`), verified against the real object,
        never leaking a raw interface. `ID3D12CommandQueue` and
        `ID3D12GraphicsCommandList` (.. `List7`) are wrapped the same way.
      - **Wrap-on-create**: `CreateCommandQueue`/`CreateCommandQueue1`,
        `CreateCommandList`/`CreateCommandList1` return wrapped queues/lists; `GetDevice`
        on a queue/list returns the device wrapper.
      - **Capture at the natural boundaries**: the list wrapper tees each recording call
        (Draw/DrawIndexed/Dispatch/DispatchMesh/ExecuteIndirect/Barrier/enhanced-Barrier/
        Clear/OMSetRT/Copy) into the rgpu protocol; `Close` finalizes; the queue wrapper
        tees + submits at `ExecuteCommandLists`, first **unwrapping** each list (via a
        private IID) back to the real object so the runtime receives real command lists.
      - **ABI-correct**: built with `WIDL_EXPLICIT_AGGREGATE_RETURNS` so aggregate-return
        vtable slots (`GetAdapterLuid`/`GetResourceAllocationInfo`/`GetCustomHeapProperties`)
        use the hidden `*__ret` pointer form that matches the MSVC-built D3D12Core exactly.
      - **~191 forwarders** are generated from the toolchain's own `d3d12.h` (regenerated
        each build) so vtable layout + ABI always match the compiler.
      - **Verified end-to-end by `test/rgpu_d3d12_harness.cpp`**: a controlled process
        creates a device via the proxy (→ wrapper), builds a clear-with-barriers list + a
        draw on wrapped objects, submits via the wrapped queue — which unwraps and submits
        the **real** list, the fence signals — and the proxy tees `execs=1 barriers=2
        clears=1 draws=1` through the wrapper. This proves the whole wrapper path
        (identity, wrap-on-create, unwrap-at-submit, ABI) is correct.

- [x] **Agility D3D12Core interception** (`src/rgpu_d3d12_proxy.cpp`). Modern UE5+Agility
      titles create their device **inside** their shipped `D3D12Core.dll`, not through the
      app-level `d3d12.dll` exports. We catch that reliably:
      - `LdrRegisterDllNotification` (documented, hook-free) fires the instant D3D12Core
        loads; we inline-hook its single export `D3D12Core!D3D12GetInterface` at the exact
        module base **before its first use** (winning the load race that polling and
        `kernel32!LoadLibraryExW` hooks lost — the Agility loader maps a private copy and
        loads via `LdrLoadDll`, whose prologue our conservative decoder can't relocate).
      - From `D3D12GetInterface` we hook the returned object **by type** (QI for
        `ID3D12SDKConfiguration1` / `ID3D12DeviceFactory`, not by hard-coded CLSID) and
        wrap any device the factory route creates.

### TXR live-capture — honest status (NOT achieved) and the exact reason

The wrapper is complete and proven (harness), and the D3D12Core interception fires
reliably, but **Tokyo Xtreme Racer's per-frame command stream is not yet captured.**
Root cause, established by live diagnosis:

- TXR (UE5 + Agility SDK **1.614**) creates its real device through a **private,
  undocumented D3D12Core bootstrap interface**: `D3D12Core!D3D12GetInterface(CLSID
  {AFEE8EAE-417D-4AC1-99A6-BDEE414E4DA2}, IID {DFAFDD2C-355F-4CB3-A8B2-EA7F9260148B})`
  returns an internal object that exposes **no public D3D12 interface**. Its vtable
  **slot 7** is the device factory (identified empirically: it emits heap `ID3D12Device`
  objects, one per `D3D12CreateDevice`).
- That slot is called **by D3D12Core itself** (`caller = D3D12Core.dll+0x79A5D`), and the
  devices it produces are consumed by **concrete D3D12Core / d3d12.dll internal code that
  reads non-COM fields at fixed offsets**. Wrapping the device there **crashes the game**
  (a COM wrapper doesn't provide the internal layout). Confirmed: observe-only is stable
  (116% GPU), wrapping-there is not.
- The devices that *do* reach the public `D3D12CreateDevice` (all four, `fl=0x1000` =
  `FL_1_0_CORE` **floor**, not a capability) get **0 `QueryInterface` and 0 command-queue
  creations** on our wrapper (`DEVICE QI HIT=0`, `DEVICE QI LEAK=0`, `wrapped-queue=0`) —
  proving they are internal capability probes, not UE5's render device. UE5's render
  device is created and used **entirely within the D3D12Core/d3d12.dll boundary** and
  never surfaces as a pure-COM `ID3D12Device` through any public entry a proxy can wrap.

**Note on the `fl` argument:** `D3D12CreateDevice`'s feature-level parameter is the
*minimum* (a floor), not the device's capability. A device made with a `FL_1_0_CORE`
floor on a 12_x GPU is still a full device — so the frontend wraps **every** non-WARP
device returned, regardless of `fl` (an earlier `fl >= 11_0` gate wrongly skipped it).

## Remaining work (to capture TXR specifically)

1. **Reach UE5's private-path device at a COM-only boundary.** Options, hardest part
   first: (a) hook `system d3d12.dll`'s internal device dispatch so the object UE5
   receives is ours while D3D12Core keeps the raw one it constructed; (b) instead of a
   COM wrapper at slot 7, vtable-hook the produced device's *own* command-creation slots
   (Draw/queue/list) so capture works without replacing the object the runtime uses; this
   sidesteps the internal-layout problem the wrapper hit.
2. **Cover `ID3D12Device11-14` / `GraphicsCommandList8-10`** for the wrapper's QI by
   vendoring Microsoft's MIT-licensed DirectX-Headers (mingw's max is `Device10`) so no
   raw revision can ever leak on a recent Agility SDK. (Not currently the blocker — TXR
   showed **0** device-QI leaks — but required for correctness on titles that do QI newer
   revisions.)
3. **Handle translation + remote** (the rgpu step-2 body, unchanged): GPU VA → resource
   id+offset; CPU/GPU descriptor handle → heap id+index; COM ptr → object id; mapped
   memory → uploaded byte ranges; fence → queue/fence id+value. Then D3D12 tee → local
   replay → **remote Windows D3D12** backend (far more practical for SM6 than Linux
   Vulkan) → Linux Vulkan last. A remote Windows D3D12 replay host is the fastest route
   to a playable result and does not depend on cracking TXR's private device path.

## Build / deploy

`build_d3d12.ps1` regenerates the wrapper forwarders, builds `test\rgpu_d3d12.dll` +
`test\rgpu_d3d12_harness.exe`, and runs the harness (the acceptance test — must print
`RESULT: D3D12 command stream captured`). Deploy: copy `rgpu_d3d12.dll` next to a D3D12
game's `.exe` as `d3d12.dll` (for TXR: `...\TokyoXtremeRacer\Binaries\Win64\d3d12.dll`),
launch with **no** `-d3d11`, then read `%TEMP%\rgpu_d3d12.log`. Remove the DLL to restore
the game.

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

- [x] **Canonical-vtable in-place interception via the ID3D12CoreModule export table**
      (`src/rgpu_d3d12_proxy.cpp`). The real device-creation route (RenderDoc-confirmed):
      `D3D12Core!D3D12GetInterface({DFAFDD2C-...} = ID3D12CoreModule)` → hook the module's
      `QueryInterface`(slot 0) + `GetDllExports`(slot 8) → on QI for the versioned
      `{FC454290-...}` interface, hook its slot 11 → it returns the **export table**, whose
      **entry 0 is the real `CreateDevice`** (`D3D12Core.dll+0x6AE20` for 1.614). We hook
      entry 0 and, on the device it makes, **patch the CANONICAL vtable in place** (not a
      wrapper, not a per-object shadow): intercept `CreateCommandQueue`/`1`,
      `CreateCommandList`/`1` (device), `ExecuteCommandLists` (queue), and the recording
      slots (list). This D3D12Core **restores an object's vtable pointer to its canonical
      vtable** (proven: a list's Draw hook only fired after re-applying on `Reset`), so
      patching the canonical vtable is what survives it — and it preserves the object's
      identity + concrete layout (the pointer is unchanged). Also acquires the render queue
      from **DXGI swap-chain creation** (`CreateSwapChainForHwnd`/etc.) and the
      **D3D11On12** present path. **Verified by the harness:** the device is created via the
      export-table `CreateDevice`, canonically patched, and the clear/barrier/draw stream is
      tee'd (`draws=1` with no re-swap, proving the canonical patch beats the restore).

### TXR live-capture — honest status (NOT achieved) and the exact reason

The mechanism is proven (harness), TXR is **stable** with it (renders at ~130% GPU, no
crash — in-place patching preserves the concrete object), and the interception fires:
`ID3D12CoreModule hooked`, `CORE_EXPORT_TABLE[00] CreateDevice hooked`, `CORE_TABLE[0]
CreateDevice` ×4, canonical DEVICE vtable patched. But TXR's per-frame stream is still not
captured. The exact reason, from live diagnosis:

- All 4 D3D12 devices share vtbl `0x…ddefb58`; the canonical patch is applied and **reads
  back live on every device** (`CFSslot`/`CCQslot` == our detours). Yet the game calls
  **neither `CheckFeatureSupport` nor `CreateCommandQueue`/`CreateCommandList`** on any of
  them across 85s of rendering. So the 4 public devices are genuinely unused at the
  public-COM level.
- TXR **presents through a D3D11On12 swap chain**: `CreateSwapChainForHwnd`'s `pDevice` is
  an `ID3D11Device` (vtbl in `d3d11.dll`, `isID3D11Device=1`), created **internally by
  d3d12.dll** (the public `D3D11On12CreateDevice` export is hooked but never called).
- Conclusion: TXR's actual command submission runs on a queue created through
  **D3D12Core-internal / D3D11On12-internal** paths that never invoke the public
  `ID3D12Device`/`ID3D12CommandQueue` COM methods a vtable hook can reach.

**Note on `fl`:** `D3D12CreateDevice`'s feature-level argument is the *minimum* (a floor),
not the device's capability — TXR passes `FL_1_0_CORE` (0x1000) yet the device is a full
12_x device. Never gate on it.

## Remaining work (to capture TXR specifically)

1. **Reach the internal queue.** The `ID3D12CoreModule` export table has ~18 entries
   (entry 0 = `CreateDevice`); hook the others to find the **internal command-queue /
   command-list / ExecuteCommandLists** entries the runtime and D3D11On12 use directly, and
   canonical-patch the queues/lists they produce. Alternatively capture at the **D3D11On12**
   layer (the game may be issuing D3D11 immediate-context draws that D3D11On12 translates to
   D3D12) — hook `ID3D11DeviceContext` recording on the D3D11On12 device.
2. **For a playable stream now:** acquire the present queue at swap-chain creation and
   capture the back buffer at `Present` (video + input), which does **not** depend on
   cracking the internal command path.
3. **Handle translation + remote** (unchanged): GPU VA → resource id+offset; descriptor
   handle → heap id+index; COM ptr → object id; mapped memory → byte ranges; fence →
   queue/fence id+value. Then tee → local replay → **remote Windows D3D12** backend → Linux
   Vulkan last.

## Full D3D12 COM wrapper (harness-verified reference)

`src/rgpu_d3d12_wrappers.h` (generated forwarders from `tools/gen_wrappers.py`) is the
complete identity-preserving `RgpuD3D12Device`/`CommandQueue`/`GraphicsCommandList`
object-graph (~191 methods, `WIDL_EXPLICIT_AGGREGATE_RETURNS` ABI). It is retained as the
protocol-serialization reference and is the right interceptor for titles that expose a
pure-COM device; it is **not** used for the TXR live path (substituting the device pointer
crashes D3D12Core's concrete-layout code — the shadow/canonical in-place path above is used
instead).

## Build / deploy

`build_d3d12.ps1` regenerates the wrapper forwarders, builds `test\rgpu_d3d12.dll` +
`test\rgpu_d3d12_harness.exe`, and runs the harness (the acceptance test — must print
`RESULT: D3D12 command stream captured`). Deploy: copy `rgpu_d3d12.dll` next to a D3D12
game's `.exe` as `d3d12.dll` (for TXR: `...\TokyoXtremeRacer\Binaries\Win64\d3d12.dll`),
launch with **no** `-d3d11`, then read `%TEMP%\rgpu_d3d12.log`. Remove the DLL to restore
the game.

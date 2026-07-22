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

### TXR live-capture — corrected status (capture not yet achieved)

The wrapper/canonical-hook mechanism remains harness-proven, and TXR remains stable while
instrumented. However, commit `cb8b089` made a diagnostic error: the generic
`ID3D12CoreModule` export-table probe restored the entire table as soon as the first
**device** was detected. Because device creation occurs near the beginning of startup, later
private-table calls could never be observed. Therefore the previous statement that all
entries were tested and produced no queue/list was not supported by the implementation.

The corrected probe now:

- keeps all discovered private-table entries hooked after device creation;
- counts calls per entry and records the first call to each entry;
- examines changed pointer out-parameters **and** direct COM-interface return values;
- stops only after a command queue is found or a bounded time/call budget expires;
- restores the original table from a timer even if the table becomes idle;
- accepts `RGPU_CET_PROBE_MS` and `RGPU_CET_PROBE_CALLS` for bounded diagnostics.

Live TXR evidence with the corrected probe shows entries `0`, `7`, `9`, `10`, `13`, and
`14` executing during startup, with entry `10` producing a device. No command queue or
command list was returned during the tested startup windows. This is a useful negative
result, but it does **not** prove that every in-process interception surface is exhausted.

The D3D11On12 test was also expanded. The earlier probe watched only `Draw` and
`DrawIndexed`; it now watches ten SDK-derived `ID3D11DeviceContext` work methods covering
direct, instanced, indirect, compute, and deferred-context submission.

The final isolated 30-second TXR run recorded an exact startup burst of **82 calls**:

```text
Dispatch=58
DrawIndexed=9
DrawIndexedInstanced=14
DrawInstanced=1
all other probed work methods=0
```

The burst included meaningful compute dimensions such as `40960x1x1`, `128x128x1`,
`240x135x1`, and graphics draws. This decisively disproves the previous claim that the
D3D11On12 immediate context never submits work. The calls were concentrated during startup
and did not reach the next 100-call telemetry milestone, so they do **not** yet prove that
sustained gameplay frames are rendered entirely through this context.

| Interception point | Current evidence |
|---|---|
| Public D3D12 device creation methods | Hooks install; no TXR queue/list creation call observed |
| Known device method implementation bodies | All four queue/list creation bodies inline-hook, including base `CreateCommandList`; none is called in the tested window |
| `ID3D12CoreModule` private export table | Corrected bounded probe observes entries 0/7/9/10/13/14; entry 10 yields a device; no queue/list seen |
| DXGI swap-chain `pDevice` | QIs as an `ID3D11Device`, not an `ID3D12CommandQueue` |
| D3D11On12 work submission | Exact startup burst: 82 compute/graphics calls; sustained per-frame role remains unproven |
| Controlled harness | Passes: `ExecuteCommandLists`, barriers, clear, and draw are captured |

The custom proxy evidence remains narrow: **its D3D12 COM/private-table probes have not
acquired TXR's live D3D12 submission object.** However, two independent findings invalidate
the earlier "D3D12Core internals are the only route" conclusion:

1. The expanded in-process probe captures real D3D11On12 compute and graphics work.
2. A bounded RenderDoc 1.44 run registered D3D12 hooks, observed the NVIDIA RTX 3050 D3D12
   device, associated a frame capturer with TXR's swap-chain window, logged `Starting
   capture`, then `Finished capture, Frame 2`, and reported DXIL use.

RenderDoc also observed a private/unknown device-interface query for GUID
`{1f052807-0b46-4acc-8a89-364f793718a4}`. Its generated `.rdc` was **not** a valid replayable
capture: the log recorded `Stream created with invalid file handle`, the thumbnail command
reported that the container had no frame capture, and a later attempt was corrupted. Invalid
containers were deleted and are not acceptance artifacts.

The next research route is therefore evidence-driven and uses standard interfaces:

- extend the acquired D3D11On12 device/context capture from work methods to the full state,
  resource, view, shader, map/update, synchronization, and presentation object graph;
- compare rgpu's D3D12 identity, private-`QueryInterface`, child-object wrapping, swap-chain
  registration, and lifetime behavior with RenderDoc's open-source wrapper;
- correlate either path with actual `Present` boundaries before declaring a sustained frame
  stream captured.

Blind proprietary `D3D12Core.dll` constructor disassembly is not justified. Concise RenderDoc
evidence is in `test/evidence/renderdoc-txr-diagnostic-20260722.txt`; the final isolated rgpu
evidence is in `test/evidence/txr-rgpu-20260722-132010.log`.

## Recovery sequence

1. Keep both controlled acceptance gates passing: the inline-hook relocation test and the
   D3D12 command-stream harness.
2. Treat the exact 82-call D3D11On12 startup burst as a proven capture surface. Expand it to
   complete D3D11 state/resource serialization and correlate calls with `Present`.
3. Compare rgpu against RenderDoc's open-source D3D12 implementation, focusing on the observed
   private interface, Agility acquisition, COM identity, child wrapping, swap-chain
   association, and lifetimes.
4. Add a TXR regression that requires work associated with repeated presented frames—not only
   startup calls—and records enough object/state data for deterministic local replay.
5. Only after a complete local replay succeeds should resource-handle translation and remote
   Windows D3D12/Vulkan replay resume. Do not guess proprietary constructor offsets.

This frontend is still a **diagnostic and serialization prototype**, not a complete remote
D3D12 runtime. A real TXR remote renderer additionally requires complete lifetime and state
coverage for resources, heaps, descriptors, root signatures, PSOs/shaders, mapped memory,
GPU virtual addresses, indirect arguments, queries, fences, residency, enhanced barriers,
and device-loss behavior.

## Full D3D12 COM wrapper (harness-verified reference)

`src/rgpu_d3d12_wrappers.h` (generated forwarders from `tools/gen_wrappers.py`) is the
complete identity-preserving `RgpuD3D12Device`/`CommandQueue`/`GraphicsCommandList`
object-graph (~191 methods, `WIDL_EXPLICIT_AGGREGATE_RETURNS` ABI). It is retained as the
protocol-serialization reference and is the right interceptor for titles that expose a
pure-COM device; it is **not** used for the TXR live path (substituting the device pointer
crashes D3D12Core's concrete-layout code — the shadow/canonical in-place path above is used
instead).

## Build / bounded TXR probe

`build_d3d12.ps1` regenerates the wrapper forwarders, builds and runs the inline-hook
test, then builds `test\rgpu_d3d12.dll` + `test\rgpu_d3d12_harness.exe` and runs the
D3D12 harness. The required acceptance lines are:

```text
RESULT: inline hooking works, survives vtable re-patching, and relocates short Jcc
RESULT: D3D12 command stream captured (ExecuteCommandLists + barrier + clear + draw tee'd)
```

Use `tools\run_txr_probe.ps1` instead of manually leaving a proxy DLL in the game folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\build_d3d12.ps1
powershell -ExecutionPolicy Bypass -File .\tools\run_txr_probe.ps1 `
  -GameRoot "C:\Program Files\Tokyo Xtreme Racer" `
  -DurationSeconds 60
```

The runner deploys the built proxy, bounds the private-table probe, captures the full log
and a filtered summary beneath `test\evidence`, stops the probe-launched game process, and
restores/removes the deployed DLL. It also recovers from an interrupted prior probe without
overwriting an unrelated pre-existing `d3d12.dll`.

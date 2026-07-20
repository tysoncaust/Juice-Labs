# rgpu-d3d12 — D3D12 interception frontend

D3D12/SM6 titles (e.g. Tokyo Xtreme Racer, UE5 with Nanite/Lumen) cannot render
on D3D11, so the D3D11 proxy can't intercept them. This is the parallel D3D12
frontend. It follows the incremental sequence: **transparent pass-through first,
serialize + remote later.**

## Status
- [x] **Step 1 — transparent pass-through proxy** (`src/rgpu_d3d12_proxy.cpp`).
      A `d3d12.dll` placed next to the game exports the loader surface a D3D12 game
      imports (`D3D12CreateDevice`, `D3D12GetInterface`, `D3D12GetDebugInterface`,
      `D3D12Serialize*RootSignature`, `D3D12Create*RootSignatureDeserializer`,
      `D3D12EnableExperimentalFeatures`) and forwards each to `System32\d3d12.dll`,
      logging the device-creation calls. **Agility SDK preserved**: because the
      forward goes to the system d3d12.dll, its loader still reads the EXE's
      `D3D12SDKVersion`/`D3D12SDKPath` exports and boots the game's own
      `D3D12\...\D3D12Core.dll` — we never touch it.
      **Verified:** Tokyo Xtreme Racer boots unchanged on native D3D12 through this
      proxy — ATTACH logged, 4× `D3D12CreateDevice` (FL 12_0) returned S_OK through
      us, game reached a responsive rendering state (GPU 3D ~93%, ~2.7 GB VRAM).

- [x] **Step 2 — command-stream tee via in-place vtable hooking** (`src/rgpu_d3d12_proxy.cpp`
      + `src/rgpu_d3d12_slots.cpp`). D3D12 has no immediate context, so we hook (PIX/
      RenderDoc-style) the shared vtables of `ID3D12CommandQueue::ExecuteCommandLists`
      (submission boundary) and the `ID3D12GraphicsCommandList` recording slots
      (Draw/DrawIndexed/Dispatch/ResourceBarrier/Clear*/OMSetRenderTargets/Copy/Close),
      arming from the game's real objects via its create methods (`CreateCommandQueue`,
      `CreateCommandQueue1`, `CreateCommandList`, `CreateCommandList1`) so the Agility
      path is covered. Slot indices are derived authoritatively from the SDK's own C
      `*Vtbl` structs (`rgpu_d3d12_slots.cpp`), not hand-counted. Each hook records the
      call into the rgpu protocol, then calls the original — game runs unchanged.
      **Verified by `test/rgpu_d3d12_harness.cpp`:** a controlled process submits a
      clear-with-barriers list + a draw and the proxy tees `execs=1, barriers=2,
      clears=1, draws=1, setRT=1` into a protocol batch. In **Tokyo Xtreme Racer** the
      hooks reach the live game (our patched command-list `Close` hook fires every
      frame), proving the vtable hooking works on the real AAA UE5 title.

      *TXR live-capture caveat (honest):* TXR bundles **UE4SS**, whose own D3D12 overlay
      re-hooks the popular vtable slots (Draw/Dispatch/Barrier/ExecuteCommandLists)
      AFTER us, superseding our hooks on those slots (Close, which mods don't touch,
      still fires). UE5 Nanite/Lumen is also compute/mesh-shader-driven, so classic
      `DrawInstanced` is rare. Capturing 100% of TXR's stream needs inline (MinHook-
      style) hooks that survive vtable re-patching, or hook-ordering vs UE4SS — bounded
      game-specific hardening, not a mechanism gap (the harness proves the mechanism).

## Remaining (step 2+ — the D3D12 body, in order)
D3D12 has no immediate context; work is recorded into command lists and submitted
through command queues, so wrapping only `ID3D12Device` is insufficient.
1. **Wrap the execution objects** (identity-preserving): `ID3D12Device` + every
   version requested via `QueryInterface`, `ID3D12CommandQueue`,
   `ID3D12CommandAllocator`, `ID3D12GraphicsCommandList` (+ later versions),
   `ID3D12Resource`, `ID3D12DescriptorHeap`, `ID3D12Fence`, `ID3D12PipelineState`,
   `ID3D12RootSignature`, and the DXGI factory/swap-chain. One wrapper identity per
   real object; `QueryInterface` returns the wrapper for all supported IIDs; wrap
   objects returned by `GetDevice`/`GetBuffer` etc. (UE5 queries newer revisions —
   returning a raw one bypasses the interceptor).
2. **Translate D3D12 handles** to protocol representations: GPU VA -> resource id +
   offset; CPU/GPU descriptor handle -> heap id + index; COM pointer -> object id;
   mapped memory -> uploaded byte ranges; fence -> queue/fence id + value. Capture
   upload-heap `Map`/`Unmap` writes and transfer modified ranges before the command
   list that references them executes.
3. **Serialize at submission**: `ID3D12CommandQueue::ExecuteCommandLists` is the
   boundary. Record while the app builds each command list, finalize on `Close`,
   submit remotely on `ExecuteCommandLists`; preserve queue type, ordering, fence
   signals/waits, and resource barriers.
4. **Diagnostics** (before device creation): enable DRED; log every requested
   device/interface IID, `CheckFeatureSupport` results, list close/submit, fence
   signal/wait, `Present` results, `GetDeviceRemovedReason`, and DRED breadcrumbs +
   page-fault output on failure (Present can return DEVICE_REMOVED/RESET).
5. **Remote**: D3D12 tee -> local replay -> **remote Windows D3D12** backend
   (substantially more practical for SM6 than Linux Vulkan) -> Linux Vulkan LAST
   (a full DXIL/SM6 + descriptor/barrier/PSO translation project of its own).

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

- [x] **Inline-hook hardening** (`src/rgpu_inlinehook.h`, `test/rgpu_inlinehook_test.cpp`).
      A minimal, conservative x64 trampoline inline hooker: a 14-byte abs-JMP at the
      function entry + a relocated prologue (RIP-relative disp32 / rel32 fixups, near-
      target trampoline). It hooks the function BODY, so it SURVIVES a later vtable
      re-patch by an overlay (UE4SS). Install is done with all other threads SUSPENDED
      (MinHook-style) so writing live D3D12Core code pages can't corrupt a running
      thread. **Verified in isolation:** the test proves the detour fires, the
      trampoline calls the original, AND the hook survives a simulated vtable re-patch
      (the UE4SS scenario). It also installs cleanly on live TXR D3D12Core functions
      without destabilising the game (thread-freezing fixed an earlier boot-instability).

      *TXR live-capture status (honest):* the tee mechanism + inline hardening are
      proven, but **full TXR capture is NOT achieved**. Live diagnosis showed our
      D3D12CreateDevice export IS called (4x) and we successfully hook that device's
      vtable — yet NONE of its methods (`CheckFeatureSupport`, `CreateCommandQueue`/`1`,
      `CreateCommandList`/`1`) ever fire while the game renders at ~110% GPU. Those 4
      devices are throwaway feature-probes; TXR's real UE5 RHI device is created +
      used through a path that bypasses the objects we intercept. Resolving it needs
      live debugging (WinDbg breakpoints on `D3D12Core!CheckFeatureSupport`) to find
      the real device object, or the deterministic-but-large full `ID3D12Device` COM
      wrapper (returned from our `D3D12CreateDevice`, all versioned interfaces) so the
      game holds OUR object and can't route around us. That is the remaining work.

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

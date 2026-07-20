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

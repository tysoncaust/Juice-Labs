# RemoteGPU — bare-metal network-backed render-only WDDM adapter

Status: architecture decision, 2026-07-23

## Decision

The production target is a **root-enumerated, render-only WDDM virtual adapter** on the bare-metal Windows client. The game remains an ordinary Windows process and selects the RemoteGPU adapter through normal DXGI/D3D12 enumeration. A Windows user-mode display driver (UMD), a displayless kernel-mode driver (KMD), and an out-of-process transport service virtualize the D3D12 device while a portable Linux executor performs equivalent work on a remote Vulkan GPU.

The existing `d3d12.dll` proxy is retained only as a bounded research and conformance tool. It is not the final game-facing deployment mechanism.

```text
Bare-metal Windows client
┌───────────────────────────────────────────────────────────────┐
│ Game / anti-cheat                                             │
│        │                                                      │
│ DXGI + D3D12 runtime                                          │
│        │ normal UMD DDI                                      │
│ RemoteGPU D3D12 UMD                                           │
│        │ local object model + canonical GPU IR                │
│ RemoteGPU render-only WDDM KMD                                │
│        │ bounded local queues / paging / fence integration    │
│ RemoteGPU transport service                                   │
└────────┬──────────────────────────────────────────────────────┘
         │ authenticated, ordered, versioned protocol
         ▼
Remote Linux executor
┌───────────────────────────────────────────────────────────────┐
│ Session / resource / queue manager                            │
│ Canonical D3D12-semantic IR → Vulkan                          │
│ Remote physical GPU                                           │
│ Readback + hardware encode                                    │
└────────┬──────────────────────────────────────────────────────┘
         │ compressed frames, fence completions, readbacks,
         │ query results, health and device-loss state
         ▼
Local decoder / presentation bridge
```

Microsoft documents render-only WDDM adapters as non-primary adapters that implement the render-specific DDIs without physical display outputs. Microsoft GPU paravirtualization also preserves the guest D3D runtime interfaces while marshalling graphics work to another execution environment. RemoteGPU adopts the same separation of responsibilities, but replaces the VM bus and host partition with a bare-metal local service and an authenticated network transport.

Primary references:

- https://learn.microsoft.com/windows-hardware/drivers/display/wddm-in-windows-8
- https://learn.microsoft.com/windows-hardware/drivers/display/wddm-driver-and-feature-caps
- https://learn.microsoft.com/windows-hardware/drivers/display/gpu-paravirtualization
- https://learn.microsoft.com/windows-hardware/drivers/display/user-mode-display-drivers
- https://learn.microsoft.com/windows-hardware/drivers/display/per-process-gpu-virtual-address-spaces
- https://github.com/HansKristian-Work/vkd3d-proton

## What the virtual adapter solves

The final adapter gives the application a normal Windows graphics-device surface:

- DXGI enumerates a real devnode-backed adapter rather than a side-loaded DLL.
- D3D12 capability negotiation happens through the Windows runtime and the RemoteGPU UMD.
- No game-directory `d3d12.dll` replacement is needed.
- No in-process injector is required by the production architecture.
- The remote executor can change between Colab, another notebook provider, or a conventional Linux GPU host without changing the game.
- The local Windows process, input stack, anti-cheat, storage, audio and networking remain on bare metal.

This is cleaner and more legitimate than API hooking, but it does **not** remove the D3D12 virtualization problem. It moves that problem into the correct driver and operating-system integration layers.

A custom graphics driver is not automatically accepted by anti-cheat. Compatibility remains a vendor-policy question and must be validated with production signing, HVCI/Secure Boot, normal installation and explicit game/vendor testing.

## Windows component boundaries

### 1. Root-enumerated device package

Use a unique hardware namespace such as:

```text
ROOT\JuiceLabs\RemoteGPU
```

The INF installs a non-primary, render-only adapter. Development begins in a disposable test machine or VM with test signing. Production distribution requires Microsoft-compatible release signing and the applicable Hardware Lab Kit path.

### 2. RemoteGPU D3D12 UMD

The UMD is the principal D3D12 semantic implementation. It must:

- expose only remotely supported feature levels, shader models, formats and options;
- maintain COM identity, object lifetime and parent/child relationships;
- translate resources, heaps, descriptors, root signatures, PSOs, command signatures, command allocators, command lists, queues, fences, queries and protected-session capability results;
- allocate local CPU-visible shadows where application memory semantics require them;
- assign synthetic GPU virtual addresses;
- build deterministic, versioned command batches;
- marshal resource data and dirty ranges;
- receive fence/query/readback completions without violating D3D12 ordering;
- fail closed with device removal when the remote session becomes unusable.

The UMD must not advertise a capability merely because the local Windows runtime recognizes it. Capability reporting is the negotiated intersection of the frontend implementation, protocol version and active backend.

### 3. Render-only WDDM KMD

“Minimal” means **displayless and narrowly scoped**, not trivial. A render-only WDDM driver still has to satisfy the required render-side WDDM contracts for the declared model and feature set.

The KMD should:

- start and stop the root-enumerated adapter;
- expose zero physical VidPn sources/targets;
- create and destroy device/context/queue-facing kernel objects;
- integrate with Dxgkrnl scheduling, paging, memory budgeting, preemption/TDR and device-removal expectations at the minimum supported feature level;
- provide bounded IOCTL/shared-section primitives for the trusted local transport service;
- expose monotonic fence progress and deterministic failure states;
- keep arbitrary network waits, DNS, TLS, compression and reconnection logic out of kernel callbacks.

Kernel callbacks must never block on a Colab round trip. They enqueue bounded local work or report a defined failure. All remote I/O occurs in user mode.

### 4. RemoteGPU transport service

The Windows service owns:

- rendezvous and authentication;
- TLS/QUIC or equivalent secure transport;
- reconnect/session epoch management;
- bounded shared-memory rings with the UMD/KMD-facing components;
- upload coalescing and compression;
- frame decode and local presentation handoff;
- telemetry, logs and watchdogs;
- clean device-removal escalation when the backend is lost.

Each ring entry has a fixed header, payload length, sequence number, session epoch and checksum. No unbounded pointers or process virtual addresses cross the trust boundary.

## Canonical remote protocol

Do not transmit vendor command buffers. They are tied to a particular driver and GPU generation. The wire protocol carries D3D12 semantics or a canonical intermediate representation.

Minimum message families:

- handshake, protocol version and backend capabilities;
- object create/destroy and reference epochs;
- heap/resource allocation and suballocation;
- upload/readback ranges;
- descriptor definitions and copies;
- shader bytecode, root signatures and PSO descriptions;
- command-list reset/close and normalized operations;
- queue submission and explicit dependencies;
- fence signal/wait/event completion;
- query resolve and readback completion;
- swap-chain image completion, encoding and frame metadata;
- residency/budget notifications;
- heartbeat, cancellation, device removal and diagnostics.

The protocol is ordered per queue, but independent uploads, command queues and frame-return traffic should eventually use separate streams to avoid head-of-line blocking.

Every object ID includes a session epoch so stale IDs from a previous Colab runtime cannot alias newly created objects after reconnection.

## The hardest semantic areas

### Persistently mapped memory

D3D12 permits applications to leave upload/readback resources mapped for long periods and modify them with ordinary CPU stores. An API-call-only transport cannot see those writes.

The client therefore needs local shadow allocations and a dirty-tracking strategy. Candidate mechanisms, in increasing order of complexity:

1. explicit dirty ranges learned from `Unmap` where the application supplies them;
2. conservative upload of all mapped upload-resource ranges referenced by a submitted list;
3. page protection/write-watch tracking for persistent mappings;
4. compiler/runtime-specific instrumentation only as an optional optimization, never a correctness requirement.

The first correct prototype should prefer conservative copies over clever but incomplete tracking. Dirty pages are snapped at queue-submission boundaries, assigned to the same submission epoch and uploaded before dependent commands.

Readback mappings require the inverse path: the client cannot expose completed bytes until the associated remote fence/query transition is satisfied.

### GPU virtual addresses

D3D12 applications obtain 64-bit GPU virtual addresses, add arbitrary byte offsets and may place the results in root descriptors, vertex/index views or indirect argument buffers. Stable object IDs alone are insufficient.

RemoteGPU assigns each buffer a non-overlapping synthetic VA range in the application's advertised GPU VA space and keeps an interval map:

```text
synthetic VA → resource ID + byte offset + generation
```

CPU-authored addresses are translated when command parameters or dirty buffer ranges are serialized. GPU-authored addresses are harder: an indirect buffer can be generated on the remote GPU and consumed later without returning to the CPU.

The architecture therefore needs one of these backend strategies, selected by negotiated capability:

- preserve the synthetic address layout remotely using Vulkan buffer-device-address features where exact address replay is supportable;
- translate shader-visible addresses through a backend address table and lower address-consuming operations accordingly;
- reject unsupported GPU-generated-address patterns in early phases instead of silently producing incorrect work.

A Phase-2 executor is not considered general until it passes CPU-authored and GPU-authored indirect-address tests.

### Descriptor heaps

Shader-visible descriptor heaps are mutable data structures with application-chosen offsets. The frontend must preserve descriptor-heap identity, CPU/GPU handle arithmetic, null descriptors, copies and aliasing. The backend reconstructs native Vulkan descriptor state or descriptor buffers from canonical descriptor records.

### Resource states and barriers

The protocol records D3D12 state transitions and enhanced barriers semantically. The Vulkan backend maps them to layouts, stages, access masks, queue ownership and synchronization. It must retain enough D3D12 history to validate split barriers, aliases and simultaneous-access resources.

### Residency and budgets

Windows exposes per-process GPU VA and residency-budget behavior. The virtual adapter must report internally consistent budgets and make `MakeResident`, `Evict`, budget notifications and over-budget failures deterministic. Backend memory pressure must be translated into the advertised local model rather than leaking arbitrary provider behavior.

### Swap chain and presentation

A render-only adapter has no physical output, so presentation needs an explicit design rather than an assumption.

Phase 0 must prove one of these supported paths:

1. render-only device plus a cross-adapter local presentation resource;
2. render-only device plus a companion indirect-display/virtual-monitor path;
3. a documented composition path where the game swap chain remains valid while completed images are supplied by the RemoteGPU service.

The chosen path must preserve windowed/fullscreen transitions, resize, color space, HDR capability reporting, frame latency, occlusion and device removal. A separate decoder window is acceptable for an early research milestone, but it is not the final transparent-adapter acceptance criterion.

## Linux/Vulkan executor

The executor is portable and provider-independent:

```text
session manager
  → resource/heap manager
  → descriptor manager
  → shader/root-signature/PSO cache
  → queue and command-list replayer
  → Vulkan synchronization and residency layer
  → readback/encoder
```

`vkd3d-proton` is the best reference and likely source of reusable translation components because it implements Direct3D 12 over Vulkan and already addresses shader translation, descriptors, barriers, ExecuteIndirect, ray tracing and many game-compatibility edge cases. It is not a network protocol, remote object model, WDDM frontend or reconnection system. RemoteGPU should reuse isolated components and design guidance where licensing and module boundaries permit, rather than attempting to fork the entire project into a network driver in one step.

The executor dials out to a stable rendezvous because notebook runtimes are ephemeral and may not accept stable inbound connections. Colab is one backend profile, not a protocol assumption.

## Development phases and acceptance gates

### Phase 0 — Windows driver feasibility spike

- Create the root-enumerated devnode in a disposable test environment.
- Enumerate a render-only adapter through DXGI/DXCore.
- Load a skeletal UMD through the normal runtime path.
- Prove `D3D12CreateDevice` can reach the RemoteGPU UMD and fail with controlled capability results.
- Select and prove the final swap-chain/presentation topology.
- Prove install, uninstall, reboot, sleep/resume and surprise removal without a bugcheck.

Exit gate: the adapter exists through normal Windows device infrastructure, has no physical outputs, and a small D3D12 test program reaches the UMD without proxy DLLs or injection.

### Phase 1 — TXR-specific semantic inventory

Use the existing proxy only for research on a non-protected configuration. Record:

- feature level, shader model and `CheckFeatureSupport` results;
- queue and command-list types;
- resource/heap types and size distributions;
- descriptor-heap sizes, mutation patterns and handle arithmetic;
- root signatures, PSO families and shader bytecode;
- persistent mappings and bytes changed per frame;
- queries, indirect execution, ray tracing, video, DML and private interfaces;
- submissions and uploads per presented frame.

Exit gate: a machine-readable TXR capability/workload profile and a replay corpus with no unsupported operation hidden behind aggregate counters.

### Phase 2 — Portable remote executor

Implement a synthetic test suite first, then the captured TXR subset:

- resource and heap creation;
- upload and readback shadows;
- descriptors;
- root signatures, shaders and PSOs;
- one direct queue and one command list;
- barriers, clears, copies, draw and dispatch;
- fence completion;
- one returned and decoded frame;
- indirect execution and synthetic GPU VA tests.

Exit gate: deterministic local and network replay of the same corpus on Vulkan, with pixel/hash and fence-order validation.

### Phase 3 — User-mode end-to-end prototype

Before kernel integration, connect the existing research frontend to the Phase-2 executor and prove the complete protocol, object model, mapped-memory tracking, GPU VA translation, frame return and failure semantics. This phase may use the proxy because it is a laboratory milestone, not the shipped design.

Exit gate: TXR reaches a defined screen through remote execution with local hardware fallback disabled, or the exact unsupported D3D12 feature is identified and reduced to a deterministic test.

### Phase 4 — Render-only WDDM adapter

Replace the proxy with the UMD/KMD/service package. Preserve the same protocol and executor. Validate:

- DXGI adapter selection;
- capability reporting;
- multiple queues and threads;
- memory pressure and residency;
- TDR/device removal;
- network loss and backend termination;
- 30-minute and 8-hour soak tests;
- installer rollback and crash-safe cleanup.

Exit gate: the game runs without side-loaded graphics DLLs or process injection, and all rendering work is attributable to the remote executor.

### Phase 5 — Production hardening and vendor compatibility

- HVCI/Memory Integrity compatibility;
- Secure Boot testing;
- Driver Verifier and HLK coverage;
- production signing and normal installer/uninstaller;
- least-privilege service account and authenticated endpoints;
- security review of all shared-memory and network parsers;
- game and anti-cheat vendor testing.

Do not describe this phase as complete merely because a test-signed driver loads. Retail trust and anti-cheat acceptance are separate gates.

## Immediate repository work order

1. Keep the corrected TXR D3D11On12/D3D12 probes and harnesses as Phase-1 instruments.
2. Add a machine-readable capability/workload schema instead of more free-form log lines.
3. Build deterministic tests for mapped upload shadows, descriptor-handle arithmetic, synthetic GPU VAs and ExecuteIndirect.
4. Split the Linux renderer into explicit resource, descriptor, PSO, queue and synchronization modules.
5. Evaluate vkd3d-proton components at module boundaries; do not transmit or replay raw native command buffers.
6. Create a separate WDK solution for the Phase-0 root-enumerated render-only adapter spike.
7. Keep Sunshine/Moonlight as a verified operational fallback, not as completion of RemoteGPU.

## Non-negotiable failure policy

- No silent local hardware fallback.
- No reuse of stale object IDs after a reconnect.
- No network blocking in kernel callbacks.
- No capability over-reporting.
- No raw vendor command buffers on the wire.
- No claim of anti-cheat compatibility without vendor validation.
- Remote-session loss becomes a defined D3D12 device-removal path.

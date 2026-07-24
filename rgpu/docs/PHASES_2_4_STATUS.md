# RemoteGPU Phases 2–4 gated status

Status date: 2026-07-24

## Acceptance policy

A phase is complete only when its defining behavior is observed in the required environment. Buildable scaffolds, software Vulkan, unsigned packages and simulated device creation are recorded separately and never promoted to production success.

## Gate 0 — architecture

The selected product requirement remains: expose a local DXGI/D3D12 render adapter whose execution is performed by a remote Vulkan backend. Ordinary remote game streaming remains the lower-risk operational fallback, but it does not satisfy the local-adapter requirement.

## Gate 1 — reproducible Vulkan execution

The Phase 2 executor implements:

- texture, render-target and readback resources;
- host-visible upload allocation and transfer;
- SPIR-V shaders and a graphics pipeline-state object;
- descriptor-set reconstruction;
- one primary command buffer and one draw;
- fence completion;
- frame readback and deterministic CRC validation;
- H.264 NVENC return;
- a separate hardware probe with device/driver/PCI/queue/extension evidence;
- Vulkan GPU timestamps;
- independent `nvidia-smi dmon` activity observation;
- explicit NVIDIA ICD forcing and software-device rejection.

Authoritative native Windows result:

```text
PHASE2_NATIVE_HARDWARE_VULKAN=PASS
DEVICE=NVIDIA GeForce RTX 3050 Laptop GPU
DRIVER=NVIDIA 610.74
GPU_TIMESTAMP_TICKS=3000855360
GPU_TIMESTAMP_NS=3000855360.000000
INDEPENDENT_MAX_SM_UTILIZATION_PERCENT=100
FRAME_CRC32=8013bd51
REFERENCE_FRAME_MATCH=PASS
NVENC_H264_RETURN=PASS
```

The selected ICD manifest and library are recorded with hashes in `out-native/phase2-native-hardware-evidence.json`. The exact NVIDIA manifest was forced through `VK_DRIVER_FILES`; the test rejects llvmpipe, lavapipe, SwiftShader, CPU and software device names. The native frame matched the prior functional reference exactly.

The WSL path remains a semantic fallback only because its Vulkan device is llvmpipe. The current Ubuntu package does not contain Mesa dzn. A strict Colab hardware script is present and a Drive notebook is generated as an additional environment, but ordinary Colab is not the authoritative baseline.

Gate 1 status: **native hardware Vulkan acceptance passed; Colab additional run pending notebook execution.**

## Gate 2 — minimum Windows adapter foundation

### Transport v2

The original two 16-slot queues were replaced with:

- a 256-entry bounded MPMC control ring;
- 16 atomically registered per-process client channels;
- one 128-entry completion ring and event per client;
- an 8 MiB bulk arena with 128 fixed 64 KiB leases;
- 128 outstanding batches and 4 MiB bulk quota per process;
- sequence numbers, connection generations, owner PIDs and owner-encoded object IDs;
- asynchronous fence-value completions;
- deliberate out-of-order completion testing;
- cancellation, reset and device-lost messages;
- stale-generation rejection;
- bounded backpressure instead of overwrite or unbounded allocation;
- CRC validation for inline and bulk data;
- networking retained entirely in the out-of-process service.

Live transport result:

```text
PHASE3_TRANSPORT_V2=PASS
MPMC_CONTROL_RING=PASS capacity=256
PER_PROCESS_COMPLETION_CHANNELS=PASS clients=16 capacity_each=128
SHARED_BULK_ARENA=PASS bytes=8388608 slots=128 slot_bytes=65536
MULTI_PROCESS_ISOLATION=PASS concurrent_clients=2
ASYNC_OUTSTANDING_BATCHES=PASS batches=128
FENCE_VALUE_COMPLETIONS=PASS
OUT_OF_ORDER_COMPLETION_MATCHING=PASS
CONNECTION_GENERATION_REJECTION=PASS
CANCELLATION_RESET_DEVICE_LOST=PASS
PROCESS_QUOTAS_AND_OBJECT_OWNERSHIP=PASS
BOUNDED_BACKPRESSURE=PASS
```

Two simultaneously active processes used distinct client slots and each completed 32 owner-bound batches without receiving the other process's completions.

### UMD and kernel components

- `OpenAdapter12` loads, registers a process channel and completes a service liveness handshake.
- The UMD still advertises no usable D3D12 interface versions and returns `DXGI_ERROR_UNSUPPORTED` from device creation.
- The generic WDK kernel broker builds and reports transport-v2 capabilities with zero kernel networking.
- A separate `RemoteGpuDxgkKmd.sys` scaffold now builds, registers through `DxgkInitialize`, reports zero display children, uses nonpaged NX-compatible allocation APIs, and fails resource/submission paths closed.

Latest build markers:

```text
UMD_OPENADAPTER12_ABI=PASS
UMD_CREATE_DEVICE=FAIL_CLOSED_NOT_IMPLEMENTED
KERNEL_BROKER_BUILD=PASS
DXGK_RENDER_MINIPORT_SCAFFOLD_BUILD=PASS
DXGK_REGISTER_PATH=DxgkInitialize
DXGK_DISPLAY_CHILDREN=0
DXGK_RESOURCE_AND_SUBMISSION_PATHS=FAIL_CLOSED_NOT_IMPLEMENTED
```

The DXGK scaffold is not installed on the main computer. An isolated Windows target with kernel debugging, crash dumps, Driver Verifier and disposable snapshots is still required before root enumeration or driver loading.

Gate 2 status: **transport v2 and DXGK build scaffold passed; live root adapter, D3D12CreateDevice, resource/copy/fence operations and service-loss device removal remain pending.**

## Gate 3 — minimum graphics implementation

Not complete. The negotiated D3D12 device-function tables for heaps, resources, descriptors, root signatures, PSOs, command lists, queues, barriers, views, draws, readback and presentation are not implemented. The UMD remains fail closed so Windows cannot mistake the current package for a working GPU.

## Gate 4 — robustness and security

Implemented foundations:

- fixed bounds and quotas;
- process-specific completion channels;
- generation-based stale-completion rejection;
- owner-bound objects and bulk leases;
- no networking or blocking network waits in kernel code;
- fail-closed service and device paths;
- installer release marker and signature enforcement.

Still required in an isolated Windows driver lab:

- Driver Verifier and code-integrity checks;
- service termination and delayed/reordered completion injection;
- TDR/reset testing;
- allocation exhaustion and protocol fuzzing;
- repeated install, upgrade, disable and uninstall cycles;
- multi-process graphics isolation;
- crash-dump and kernel-debug review.

## Gate 5 — certification and packaging

Implemented:

- Display-class INF;
- Inf2Cat with zero errors and zero warnings;
- normal validate/install/uninstall executable paths;
- WinVerifyTrust validation;
- no test-signing dependency in the release package;
- fail-closed refusal without Microsoft signatures and a production-ready marker;
- documented service and network endpoints;
- Secure Boot/HVCI status collection.

Current host observation:

```text
HVCI configured: false
HVCI running: false
VBS status: 2
Secure Boot query: access denied without elevation
```

Microsoft production signing, HLK/WHCP, Secure Boot and HVCI acceptance require external organization credentials, certificates, Partner Center submission and dedicated signed-package test systems.

## Gate 6 — applications and vendors

Not started. Controlled D3D12 samples must pass before engines, ordinary games or anti-cheat-protected titles. Game and anti-cheat support remains unsupported until verified per title/vendor; there is no universal anti-cheat approval.

## Overall status

```text
GATE1_NATIVE_HARDWARE_VULKAN=PASS
GATE1_COLAB_ADDITIONAL_RUN=PENDING_NOTEBOOK_EXECUTION
GATE2_TRANSPORT_V2=PASS
GATE2_DXGK_MINIPORT_SCAFFOLD_BUILD=PASS
GATE2_ROOT_ADAPTER_LIVE_TEST=PENDING_ISOLATED_WINDOWS_TARGET
GATE2_D3D12CREATEDEVICE=PENDING_DDI_IMPLEMENTATION
GATE3_MINIMUM_GRAPHICS_IMPLEMENTATION=PENDING
GATE4_ROBUSTNESS_SECURITY=PENDING_ISOLATED_DRIVER_TESTING
GATE5_CATALOG_SETUP_TOOLING=PASS
GATE5_PRODUCTION_SIGNATURE_HLK=PENDING_EXTERNAL
GATE6_VENDOR_VALIDATION=PENDING_EXTERNAL
FINAL_PRODUCT_READY=FALSE
```

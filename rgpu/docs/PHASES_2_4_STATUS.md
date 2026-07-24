# RemoteGPU Phases 2–4 status

Status date: 2026-07-24

## Acceptance policy

A phase is marked complete only when its defining system works in the requested environment. A buildable skeleton, software fallback, unsigned catalog, or simulated service is not reported as production completion.

## Phase 2 — remote Vulkan executor

Implemented under `rgpu/renderd/linux/phase2/`:

- Vulkan resource creation for an uploaded texture, render target and readback buffer;
- host-visible upload allocation and buffer-to-image transfer;
- SPIR-V vertex and fragment shaders;
- descriptor-set layout, pool, allocation and combined-image-sampler reconstruction;
- render pass, framebuffer, pipeline layout and graphics PSO;
- one primary command buffer and one draw;
- fence submission and bounded completion wait;
- BGRA/RGBA frame readback, CRC and non-uniform-pixel validation;
- H.264 compressed return with NVENC preferred and a strict NVENC-only Colab gate.

Latest live WSL2 functional result:

```text
PHASE2_REMOTE_EXECUTOR=PASS
GPU=llvmpipe (LLVM 21.1.8, 256 bits)
HARDWARE_VULKAN=false
RESOURCE_CREATION=PASS
SHADER_AND_PSO_CREATION=PASS
UPLOAD_HEAP=PASS
DESCRIPTOR_RECONSTRUCTION=PASS
COMMAND_LIST=PASS
RENDERED_FRAME=PASS
FENCE_COMPLETION=PASS
COMPRESSED_FRAME_RETURN=PASS
ENCODER=h264_nvenc
```

This proves the complete semantic path and hardware NVENC return, but not hardware Vulkan rendering: the current WSL Vulkan loader exposes only llvmpipe. The strict runner `rgpu/colab/run_phase2_colab.py` requires both a non-CPU Vulkan physical device and `h264_nvenc`. No Colab endpoint is currently allocated, so the expanded shader/PSO test has not received a fresh T4 run.

Phase 2 status: **functional implementation complete; requested Colab hardware acceptance pending a live Colab GPU session.**

## Phase 3 — WDDM components and transport

Implemented under `rgpu/wddm/phase3/`:

- fixed-size 16-slot request and completion shared-memory queues;
- version, size and CRC validation for every message;
- immediate fail-closed behavior when a queue is full;
- request and completion events with bounded waits;
- an out-of-process transport service owning the remote/network boundary;
- a D3D12 UMD DLL exporting `OpenAdapter12`;
- UMD-to-service liveness handshake over the bounded queues;
- adapter function-table population and a live ABI test;
- fail-closed `CreateDevice` until the mandatory D3D12 DDI body exists;
- a WDK-built x64 kernel broker exposing a bounded ABI query;
- zero socket/network operations and zero blocking network waits in kernel code.

Latest live result:

```text
PHASE3_TRANSPORT=PASS
BOUNDED_SHARED_MEMORY_QUEUES=PASS capacity=16
OUT_OF_PROCESS_SERVICE=PASS
UMD_OPENADAPTER12_ABI=PASS
UMD_TO_SERVICE_ROUNDTRIP=PASS
KERNEL_BROKER_BUILD=PASS
KERNEL_NETWORK_OPERATIONS=0
KERNEL_BLOCKING_NETWORK_WAITS=0
UMD_CREATE_DEVICE=FAIL_CLOSED_NOT_IMPLEMENTED
ROOT_ENUMERATED_RENDER_ADAPTER=NOT_COMPLETE
D3D12_GRAPHICS_DDI_BODY=NOT_COMPLETE
```

The `.sys` is a real WDK-built kernel broker, but it is not yet a DXGK render miniport. The UMD is a real loadable D3D12 entry-table component, but it intentionally advertises no supported DDI versions and refuses device creation because resource, queue, command-list, residency and synchronization DDIs are not complete. Installing the current Display-class INF would therefore be unsafe and is blocked by Phase 4 validation.

Phase 3 status: **transport, UMD ABI and kernel build complete; usable root-enumerated D3D12 render adapter not complete.**

## Phase 4 — production packaging and external approval

Implemented under `rgpu/wddm/phase4/`:

- Display-class root-enumerated INF package;
- WDK Inf2Cat generation with zero errors and zero warnings;
- normal `RemoteGpuSetup.exe` validate/install/uninstall command paths;
- WinVerifyTrust validation for SYS, DLL, service EXE and CAT;
- installer readiness marker requirement;
- non-elevated mutation refusal;
- no test-signing dependency in the package or build;
- documented service, shared-memory and future network endpoints;
- machine-readable production-readiness manifest;
- Secure Boot and Device Guard/HVCI status collector.

Latest package validation:

```text
CATALOG_GENERATION=PASS_UNSIGNED
PHASE4_INSTALLER_BUILD=PASS
PHASE4_UNINSTALLER_BUILD=PASS
PHASE4_PACKAGE_VALIDATION=REFUSED
missing:production-ready.marker
signature-invalid:RemoteGpuKmd.sys
signature-invalid:RemoteGpuUmd.dll
signature-invalid:RgpuTransportService.exe
signature-invalid:RemoteGpuRoot.cat
```

Current host security observation:

```text
HVCI configured: false
HVCI running: false
VBS status: 2
Secure Boot query: access denied without elevation
```

Production signing cannot be generated locally without the external organization/certificate/submission requirements. HLK, Secure Boot/HVCI compatibility, game-vendor review and anti-cheat-vendor approval also require external infrastructure or counterparties. The package deliberately has no `production-ready.marker`, so `--install` cannot mutate the system.

Phase 4 status: **package and fail-closed release tooling complete; production signing, HLK/security acceptance and vendor approval not complete.**

## Overall conclusion

The repository now contains executable and repeatable implementations for the Phase 2 semantic pipeline, Phase 3 bounded service/UMD/kernel foundation and Phase 4 release tooling. The requested product is not 100% complete because three acceptance gates remain materially unmet:

1. fresh hardware Vulkan execution of the expanded Phase 2 test on Colab;
2. the full DXGK render miniport and complete D3D12 graphics DDI body;
3. Microsoft production signing, HLK/HVCI/Secure Boot validation and vendor approval.

None of these are replaced by a false success marker or test-signing workaround.

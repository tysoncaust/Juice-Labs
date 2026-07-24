# RemoteGPU WDDM Phase 0 feasibility spike

This directory contains buildable Phase-0 artifacts for the render-only virtual GPU architecture.

## What this proves now

- a versioned, bounded shared-memory submission/completion ABI;
- an out-of-process transport boundary that keeps network work out of kernel callbacks;
- a deterministic frame-return and presentation contract;
- a Windows presentation proof using a normal local window and BGRA frame buffers;
- a root-enumerated driver-package skeleton whose INF can be inspected without installing it;
- fail-closed validation and protocol tests.

## What this does not claim

It is not yet a functioning WDDM display miniport or D3D12 UMD. Building and loading those requires the Windows Driver Kit, a driver test machine or VM, test signing for development, and eventually production signing/HLK validation. The package is deliberately not installed by the build script.

## Build and test

```powershell
powershell -ExecutionPolicy Bypass -File .\build_phase0.ps1
```

The acceptance test creates an in-memory command queue, executes a synthetic clear remotely, returns a BGRA frame, validates its CRC, and exercises queue saturation/fail-closed behavior. `--window` additionally presents animated returned frames in a Win32 window.

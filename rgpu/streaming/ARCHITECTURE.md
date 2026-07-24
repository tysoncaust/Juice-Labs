# Bare-metal streaming architecture

## Product claim

This project is an open-source, stock-driver remote streaming implementation. It reports observed, version-specific compatibility. It does not claim universal anti-cheat compatibility or vendor approval.

## Security boundary

The Windows host is an ordinary user process and must preserve these invariants:

- `NO_DLL_INJECTION=TRUE`
- `NO_GAME_PROCESS_HANDLES=TRUE`
- `NO_GAME_MEMORY_ACCESS=TRUE`
- `NO_GAME_WINDOW_HOOKS=TRUE`
- `NO_OVERLAY=TRUE`
- `NO_CUSTOM_DISPLAY_DRIVER=TRUE`
- `NO_KERNEL_DRIVER=TRUE`
- `STOCK_WINDOWS_CAPTURE_APIS_ONLY=TRUE`

## Components

1. **Windows host agent** captures the ordinary desktop and default render endpoint, encodes video with stock NVIDIA NVENC and audio with AAC or Opus, and sends encrypted media.
2. **SRT reference path** provides AES-256 authenticated transport, retransmission, VLC/native-client interoperability, and deterministic impairment testing.
3. **WebRTC browser path** uses DTLS-SRTP media, ICE, STUN/TURN, browser statistics, and a PWA receiver. The checked-in PWA is passive-first; the native WebRTC media sender remains a separately tracked implementation gate.
4. **Compatibility registry** records exact game, anti-cheat, launcher, Windows, GPU-driver, capture-backend, input-mode, and host-agent versions.

## Passive and interactive products

### Passive protected-game streaming

Video and audio are available. Input code is absent from the passive release bundle. The passive build is the default.

### Interactive remote control

Interactive input is a separate explicit build target. It is never included in the passive package. Human input only is permitted; macros, programmable timing, replay, turbo, background injection, game-process inspection, and automation are prohibited. Per-title interactive results are separate from passive results.

## Non-goals

- Vendor endorsement or universal BattlEye compatibility.
- A custom WDDM adapter for protected games.
- Game automation, anti-cheat bypass, process inspection, overlays, or kernel components.

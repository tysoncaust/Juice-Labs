# Bare-metal protected-game streaming compatibility prototype

Status date: 2026-07-24

This directory is the low-risk go/no-go path for games protected by anti-cheat systems. It deliberately uses an ordinary bare-metal Windows installation, the stock NVIDIA driver, ordinary desktop capture, NVENC, and a display/input-only remote client.

It does **not** install or expose the RemoteGPU WDDM adapter. It contains no game DLL injection, process-memory access, anti-cheat modification, concealment, virtual machine spoofing, or detection bypass.

## Architecture

```text
Unmodified game on bare-metal Windows
        |
        | ordinary desktop pixels
        v
Windows desktop capture (GDI proof path)
        |
        | CPU-visible BGRA frames
        v
Stock NVIDIA h264_nvenc on RTX 3050
        |
        | MPEG-TS / UDP
        v
MacBook display endpoint (VLC proof client)
```

Input is kept separate from video. The input proof uses an outbound Windows-to-Mac TCP connection so no inbound Windows firewall rule is required. The Windows relay launches its own `RGPU Input Test` process, verifies that the destination window belongs to that exact child PID, and only then uses `SendInput`. There is no option to name or target an arbitrary process.

Protected-game input is not automated. The Local File MCP game guard remains authoritative and refuses input or memory automation for protected processes.

## Files

- `host_nvenc_stream.ps1` — bounded desktop capture to H.264 NVENC and UDP, with `nvidia-smi dmon` evidence.
- `mac_vlc_receiver.sh` — bounded MPEG-TS/H.264 receiver and decode-marker validator.
- `input_client.py` — one-shot Mac input endpoint.
- `input_relay_host.cpp` — outbound Windows relay restricted to its own child test window.
- `input_test_window.cpp` — owned target used to prove remote keyboard delivery.
- `run_passive_game_capture.ps1` — protected-game process-survival and capture-to-null validator; retains no video.
- `build_compatibility.ps1` — builds the benign input proof with MSVC `/W4 /WX`.
- `validate_compatibility.ps1` — validates the build and the latest evidence artifacts.
- `compatibility-matrix.json` — machine-readable result matrix and custom-driver gate.

## Live results on this host

### Generic bare-metal path

```text
BAREMETAL_REMOTE_VIDEO=PASS
CAPTURE=gdigrab
ENCODER=h264_nvenc
REQUESTED_MODE=1280x720@24
ENCODED_FRAMES=309
MAX_NVENC_UTILIZATION_PERCENT=6
MAC_LAN_VIDEO_RECEIVER=PASS
RGPU_REMOTE_INPUT_TEST=PASS
```

The Mac receiver observed MPEG-TS, H.264 and decoded-video output markers. The remote input test delivered `KEY_A` only to the relay-owned test window.

### Destiny 2

Destiny 2 was launched through Valve's signed Steam launcher using app ID `1085660`. The running `destiny2.exe` PID was identified by the local game guard as BattlEye-protected, and input/memory automation was refused.

```text
PASSIVE_GAME_CAPTURE=PASS
GAME=Destiny 2
SAME_PROCESS_ALIVE=true
BATTLEYE_SERVICE=Running
ENCODED_FRAMES=453
MAX_NVENC_UTILIZATION_PERCENT=7
VIDEO_RETAINED=false
REMOTE_INPUT_ATTEMPTED=false
MAC_LAN_VIDEO_RECEIVER=PASS
```

This proves that the game could remain running with BattlEye active while ordinary desktop capture and stock NVIDIA NVENC operated, and that the resulting target-session stream reached and decoded on the Mac.

It does **not** prove:

- successful online matchmaking;
- vendor support for this exact complete software stack;
- remote keyboard, mouse or controller input inside Destiny 2;
- reconnection or dynamic resolution changes during Destiny 2;
- universal compatibility with BattlEye or other anti-cheat products.

Those items require manual designated-account testing and, where applicable, written game/anti-cheat vendor guidance.

## Hybrid-GPU finding

This laptop's physical desktop is owned by Intel UHD Graphics, while the RTX 3050 is render-only and has no attached DXGI output. Sunshine therefore falls back to Quick Sync in the normal service configuration. An isolated `capture = wgc`, `encoder = nvenc`, RTX-selected Sunshine instance also failed to locate an output device.

Sunshine documents `adapter_name`, `encoder = nvenc`, Windows Desktop Duplication, and beta Windows Graphics Capture; it also states that WGC is not compatible with the Sunshine service. The topology-independent proof in this directory avoids that coupling by capturing CPU-visible desktop pixels and uploading them to stock NVENC.

## Build

```powershell
.\build_compatibility.ps1
```

## Video test

Start the Mac receiver:

```sh
./mac_vlc_receiver.sh 50000 60 /private/tmp/rgpu-compatibility
```

Then start the Windows host:

```powershell
.\host_nvenc_stream.ps1 `
  -DestinationAddress 192.168.0.45 `
  -Port 50000 `
  -DurationSeconds 15 `
  -FramesPerSecond 24
```

The host does not retain the transmitted video.

## Owned-window input test

On the Mac, start the one-shot server with a fresh token:

```sh
python3 input_client.py --port 50001 --token '<random-token-at-least-16-characters>'
```

On Windows, run the relay against the compiled test window:

```powershell
.\out\rgpu_input_relay_host.exe `
  --server 192.168.0.45 `
  --port 50001 `
  --token '<same-token>' `
  --test-window .\out\rgpu_input_test_window.exe `
  --marker .\out\input-marker.txt
```

The relay has no general target-process argument. It launches and binds to its own test window.

## Passive protected-game capture

Launch the title normally through its supported store launcher. After the genuine game process appears:

```powershell
.\run_passive_game_capture.ps1 `
  -GameProcessName destiny2 `
  -GameDisplayName 'Destiny 2' `
  -DurationSeconds 20
```

The script observes process continuity and encodes to FFmpeg's null muxer. It does not retain video or send input.

## Product decision

The custom WDDM path remains a separate research project. For protected games, it must not advance to target-game testing unless both conditions are established:

1. the product fundamentally requires an unmodified client application to enumerate a local-looking D3D12 adapter; and
2. the relevant game and anti-cheat vendors indicate that this architecture can be supported.

Current result:

```text
BARE_METAL_REMOTE_RENDERING=OPERATIONAL_PROTOTYPE
CUSTOM_DRIVER_REQUIRED_FOR_STREAMING=false
VENDOR_SUPPORT_FOR_CUSTOM_ADAPTER=false
PROCEED_WITH_CUSTOM_DRIVER_FOR_PROTECTED_GAMES=false
```

The appropriate next work is latency, audio, encrypted/reliable transport, reconnection, resolution negotiation, manual gameplay/matchmaking validation, and vendor engagement—not virtual-adapter installation.

## Public policy references

- Bungie BattlEye support guide: https://help.bungie.net/hc/en-us/articles/4404072197140-BattlEye-Anti-Cheat-Support-Guide
- Bungie third-party application compatibility: https://help.bungie.net/hc/en-us/articles/360049199891-Destiny-2-PC-and-Third-Party-Application-Feature-Compatibility
- Sunshine configuration: https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2configuration.html

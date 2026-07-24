# Playable Tokyo Xtreme Racer remote stream

This is the production-feasible path while the experimental D3D12 virtualization frontend remains incomplete.

## Architecture

Run the complete Windows game, D3D12 Agility runtime, and display driver together on a Windows GPU host. Sunshine captures and hardware-encodes the host display; Moonlight returns video/audio and forwards keyboard, mouse, and controller input. This avoids pretending that an incomplete D3D12 command tee is already a remote graphics driver.

Google Colab remains useful for synthetic Vulkan/NVENC/protocol experiments. It is not a production host for the closed-source Windows game: the managed runtime is Linux and ephemeral, while TXR requires its Windows executable, D3D12 runtime, driver stack, persistent installation, and interactive session to remain together.

## Verified local host/client state

- `SunshineService` is installed, automatic, and running on the Windows host `Laptop`.
- The paired client is a MacBook with Moonlight installed at `/Applications/Moonlight.app`.
- Moonlight can resolve `Laptop`, authenticate with the existing pairing, and retrieve the live application list.
- A bounded live session successfully initialized control, video, audio, and input streams.
- Requested stream mode: 1920x1080, 60 fps, H.264, 15 Mbps.
- Sunshine encoded with Intel Quick Sync (`h264_qsv`) because the active display is on Intel UHD Graphics.
- Moonlight decoded with Apple VideoToolbox and rendered with Metal on Apple M1.
- TXR remained alive for the full 25-second validation window while the stream was active.
- Evidence: `evidence/moonlight-txr-e2e-20260722.txt` and `evidence/sunshine-live-session-20260722.log`.

The current live Sunshine configuration still requests NVENC first. NVENC is unsupported on the active Intel display GPU, so Sunshine falls back to Quick Sync. The staged `sunshine.conf` selects Quick Sync directly and avoids the failed NVENC probe. Applying that file under `Program Files` requires an administrator or Sunshine's authenticated web manager; it is an optimization, not a blocker.

## Ready-to-use launchers

Windows host launcher:

```text
C:\Users\email\AppData\Local\Programs\Tokyo Xtreme Racer Remote\Tokyo Xtreme Racer - Remote.cmd
```

A searchable Start Menu entry is installed at **Juice Labs > Tokyo Xtreme Racer Remote**. The launcher validates the game path and refuses to launch if an rgpu diagnostic `d3d12.dll` was accidentally left beside the game executable.

MacBook launcher:

```text
/Users/macbook/Applications/Tokyo Xtreme Racer Remote.command
```

It opens a fullscreen 1080p/60 H.264 Moonlight Desktop stream to host `Laptop`.

## Start a playable session

1. Open the MacBook **Applications** folder and double-click **Tokyo Xtreme Racer Remote.command**.
2. In the streamed Windows desktop, open Start, search for **Tokyo Xtreme Racer Remote**, and launch the entry under **Juice Labs**.
3. Play normally. Closing Moonlight ends the client session; closing TXR leaves the Desktop stream available because it was launched through Sunshine's Desktop application.

Equivalent Moonlight CLI command:

```sh
/Applications/Moonlight.app/Contents/MacOS/Moonlight stream \
  --1080 --fps 60 --bitrate 15000 \
  --video-codec H.264 --video-decoder hardware \
  --display-mode fullscreen --no-quit-after \
  Laptop "Desktop"
```

## Optional dedicated Moonlight tile

A dedicated **Tokyo Xtreme Racer** Sunshine tile is staged but was not applied automatically because the active MCP profile cannot perform UAC/admin writes under `C:\Program Files\Sunshine` and the Sunshine password SecretRef is locked.

Files:

- `sunshine-txr-app.json`: single-app request/template.
- `../../docs/sunshine-apps-with-txr.json`: complete merged `apps.json` using the guarded launcher.
- `sunshine.conf`: verified Quick Sync encoder configuration.
- `install_sunshine_txr.cpp` and `build_installer.ps1`: narrow elevated installer. It backs up both live files, replaces only Sunshine's app/config files, restarts `SunshineService`, and rolls back if restart fails.

The dedicated tile is convenience only. The tested Desktop-plus-launcher flow is already operational end to end.

## Measured bounded test

Moonlight reported:

- first video packet after 200 ms;
- 0.00% network frame loss;
- 0.15% jitter drops;
- 3 ms average network latency, 1 ms variance;
- 10.3 ms average host processing latency;
- 4.22 ms average hardware decode time;
- 1.58 ms average render time;
- stereo audio initialized and received.

The 33.84 FPS session-wide average includes static desktop/startup periods and does not mean the negotiated mode changed; the client and host both logged a 1920x1080x60 video stream.

## What remains experimental

This streaming path runs the game on the Windows host. It is not the separate research goal where the game process runs on a CPU-only Windows client while a remote GPU executes a virtualized D3D12 object/command stream.

That research path still requires complete resource, descriptor, GPU virtual-address, shader/PSO, synchronization, residency, lifetime, query, indirect-command, and device-loss semantics. See `../../docs/REMOTE_WDDM_ARCHITECTURE.md`, `../../docs/TXR_RECOVERY_PLAN.md`, and `../../proxy-d3d12/README.md`.

## Protected-game compatibility path

For stock-driver, bare-metal streaming tests that do not install the custom WDDM adapter, see [`compatibility/`](compatibility/README.md). That prototype uses ordinary desktop capture, RTX NVENC, a Mac display endpoint, an owned-window-only input proof, and passive anti-cheat game validation.

# Bare-metal protected-game streaming

Status date: 2026-07-24

This directory implements the stock-driver path and reports observed, version-specific compatibility. It does not claim vendor approval, endorsement, certification, general BattlEye compatibility or universal anti-cheat compatibility.

## Hard boundaries

`NO_DLL_INJECTION=TRUE`, `NO_GAME_PROCESS_HANDLES=TRUE`, `NO_GAME_MEMORY_ACCESS=TRUE`, `NO_GAME_WINDOW_HOOKS=TRUE`, `NO_OVERLAY=TRUE`, `NO_CUSTOM_DISPLAY_DRIVER=TRUE`, `NO_KERNEL_DRIVER=TRUE`.

## Product modes

- **Passive** is the default build. It contains desktop video and WASAPI audio components and does not compile or package an input broker.
- **Interactive** is an explicit separate build target for human input experiments. It is untested for Destiny 2 and is never implied by a passive result. Macros, automation, replay and programmable timing are prohibited.

Build passive media components:

```powershell
.\build_compatibility.ps1 -Mode Passive
```

Build the separately gated benign interactive proof:

```powershell
.\build_compatibility.ps1 -Mode Interactive
```

## Observed results

- Real Windows default-endpoint WASAPI loopback: pass.
- AES-256 SRT H.264 NVENC + AAC to Mac VLC: pass.
- Incorrect-key rejection: pass.
- Session reconnect and 1280x720 -> 1600x900 mode change: pass.
- Seeded 3% loss / 50 ms jitter smoke: pass for one 30-second media run; full ten-minute, three-seed matrix is not complete.
- Destiny 2 passive launch/capture/BattlEye continuity: observed pass on the recorded stack.
- Destiny 2 manual gameplay, matchmaking, PvP and interactive remote input: not tested.

## Web client

`../../web/pwa` is an installable passive WebRTC receiver scaffold. It creates receive-only media transceivers, rejects every data channel, collects browser WebRTC statistics and uses `requestVideoFrameCallback()` for compositor-level timestamps. The native Windows WebRTC sender is not yet implemented, so SRT remains the tested reference transport.

## Release evidence

`validate_compatibility.ps1` verifies the passive boundary and canonical evidence. `build_release_bundle.ps1` creates a passive-only prerelease archive, checksums and SPDX SBOM. The GitHub workflow is pinned and configured for artifact attestations, but no attestation or signed release is claimed until the workflow actually runs.

See `../../../ARCHITECTURE.md`, `../../../THREAT_MODEL.md`, `../../../COMPATIBILITY.md`, `../../../EVIDENCE_SCHEMA.md`, `../../../SECURITY.md` and `mode-policy.json`.

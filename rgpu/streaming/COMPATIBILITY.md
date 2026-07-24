# Observed compatibility registry

Compatibility means only that the recorded version-specific test completed without the listed warning, kick, crash, or account action. It is not approval, certification, endorsement, or a universal anti-cheat statement.

## Status vocabulary

- `OBSERVED_COMPATIBLE` — the exact recorded stack passed the stated procedure.
- `PREVIOUSLY_OBSERVED_COMPATIBLE` — a material component changed; retest required.
- `UNTESTED` — no valid observation exists.
- `OBSERVED_INCOMPATIBLE` — the recorded stack failed or was blocked.
- `NOT_APPLICABLE` — the mode or test does not apply.

## Current registry

| Title | Mode | Observed result | Manual gameplay | Matchmaking | Expiry rule |
|---|---|---|---|---|---|
| Destiny 2 / BattlEye | Passive | Observed launch, BattlEye continuity, desktop capture and remote decode passed on the recorded 2026-07-24 stack | Not tested | Not tested | Retest after any material component change |
| Destiny 2 / BattlEye | Interactive | Untested | Not tested | Not tested | No claim |

## Required record dimensions

Every published observation must include game and anti-cheat versions when obtainable, launcher version, Windows build, GPU and driver, capture backend, input mode, host-agent commit/binary hash, test operator type, limitations and artifact hashes.

## Vendor communication

Vendor responses may be attached as additional evidence. They are optional and non-blocking. Absence of a response does not prevent an observational release, and no response is represented more broadly than its exact wording and scope.

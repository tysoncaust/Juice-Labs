# Threat model

## Assets

Session credentials, media confidentiality, host input authority, compatibility evidence integrity, release artifacts, and user account safety.

## Trust boundaries

- Browser/PWA to signaling service.
- Browser/PWA to WebRTC peer.
- Native client to SRT listener.
- Host media process to Windows capture and audio APIs.
- Optional interactive input broker to the foreground desktop session.
- CI build to released artifacts and attestations.

## Primary threats and controls

| Threat | Control |
|---|---|
| Session theft | Short-lived session tokens, origin checks, DTLS-SRTP/SRT encryption, no credentials in URLs or logs |
| Cross-site signaling abuse | HTTPS/WSS, explicit allowed origins, CSRF-resistant session creation, bounded message schemas |
| Unauthorized input | Passive build has no input binary; interactive mode requires local confirmation, foreground ownership, visible indicator, rate limits and emergency release |
| Stuck controls | Release-all on disconnect/focus loss, periodic complete state snapshot, bounded channel lifetime |
| Game-process inspection | No process handles, memory APIs, injection, hooks or overlays; static and runtime boundary tests |
| Secret leakage | Environment/secret-store references, redacted logs, repository scanning, hashed evidence identifiers only |
| Evidence tampering | JSON schema, file hashes, commit binding, release checksums, SBOM and GitHub artifact attestations |
| Dependency compromise | Pinned dependencies and actions, SBOM, update review, minimal dependency surface |
| Denial of service | Packet, bitrate, session and resource quotas; bounded waits; reconnect backoff |
| Compatibility overclaim | Versioned observations expire on material version changes and never become vendor certification |

## Anti-cheat-specific policy

Passive and interactive compatibility are independent. A passive pass never implies an interactive pass. A result is invalidated when the game executable, anti-cheat client, launcher, Windows build, GPU driver, capture backend, input backend, or host-agent version materially changes.

## Residual risks

Publishers may block ordinary capture or input software at any time. Browser and GPU-driver behavior changes. A clean local test cannot prove future compatibility. Physical input-to-photon latency requires external measurement hardware and is not inferred from software timestamps.

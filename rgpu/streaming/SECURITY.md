# Security policy

Report vulnerabilities privately to the repository maintainers before public disclosure. Include affected commit, mode, reproduction steps, impact, and whether the issue crosses the passive/interactive boundary. Do not test against third-party game services or accounts without authorization.

## Release requirements

- Passive releases contain no input broker.
- No game-process handles, memory access, injection, hooks, overlays, custom display driver or kernel driver.
- Secrets are supplied at runtime and are absent from source, logs and evidence.
- Release checksums, SPDX SBOM and provenance attestations are published when the GitHub release workflow runs.
- Web/PWA review uses OWASP ASVS as the requirements baseline.

## Supported versions

Security fixes target the latest prerelease and current main branch until a stable release policy is published.

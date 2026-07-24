# RemoteGPU certification plan

## Release organization

These items require an authorized legal organization and cannot be completed by repository automation:

- designate the publishing legal entity and Microsoft Entra tenant;
- obtain an EV code-signing certificate from an accepted certificate authority;
- register and verify a Windows Hardware Developer Program account;
- associate the certificate with the Partner Center hardware account;
- define separate development, preproduction and production signing identities;
- establish certificate custody, rotation, revocation and incident procedures.

## Submission strategy

The target quality bar is HLK/Windows Hardware Compatibility Program testing for the final graphics package. Attestation signing, where eligible, is not treated as equivalent to Windows certification.

Submission inputs must include:

- final release INF/CAT/SYS/UMD/service/installer;
- release symbols;
- hardware IDs and supported Windows build matrix;
- HLK package and playlist results;
- HVCI/readiness results;
- organization and certificate metadata;
- versioned threat model and security response contact.

## Secure Boot, VBS and HVCI matrix

Final acceptance must use the actual Microsoft-signed package on dedicated systems with:

- Secure Boot enabled;
- VBS enabled;
- Memory Integrity/HVCI enabled;
- Driver Verifier code-integrity checks enabled;
- supported Windows releases and update levels;
- clean install, upgrade, rollback, disable and uninstall cycles.

Required implementation constraints include:

- no writable-and-executable kernel pages;
- no executable pool allocations;
- no dynamic code generation or code modification;
- NX-compatible pool APIs and correct section protections;
- strict validation of all user-mode/shared-memory input;
- no embedded user pointers in shared protocol messages;
- bounded arithmetic for every offset and size;
- deterministic reset/device-removal behavior after service loss.

## Test lab gates before submission

1. Root-enumerated adapter loads in a disposable Windows target.
2. `D3D12CreateDevice` succeeds only after all advertised DDIs work.
3. Upload/copy/readback and fence tests pass.
4. Triangle render/readback matches a reference image.
5. Service loss causes bounded device removal or reset without a hang or bugcheck.
6. Driver Verifier remains clean.
7. Protocol fuzzing produces no kernel memory-safety failure.
8. TDR, delayed/reordered completion and allocation-exhaustion tests pass.
9. Upgrade/uninstall removes the device, service, files and driver-store package cleanly.

## External status

Until all submission and signed-package tests are complete:

- production signing: pending external organization/certificate/submission;
- Windows certification: not obtained;
- Secure Boot/HVCI acceptance: not complete;
- game support: unsupported;
- anti-cheat compatibility: unsupported.

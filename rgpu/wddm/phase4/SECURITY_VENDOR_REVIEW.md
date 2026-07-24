# RemoteGPU security and vendor review package

Status: engineering draft; not a vendor approval claim.

## Trust boundaries

1. **Game/application process** — uses standard DXGI/D3D12 runtime entry points.
2. **RemoteGPU UMD** — validates D3D objects and serializes only the negotiated protocol.
3. **DXGK KMD** — owns Windows graphics lifecycle, scheduling, memory and synchronization objects. It performs no networking and accepts no credentials.
4. **Transport service** — owns shared-memory consumption, authenticated remote connections, encryption, reconnection and protocol translation.
5. **Remote executor** — validates the protocol again and executes Vulkan work in an isolated process/container.

## Kernel attack-surface rules

- No writable-and-executable kernel memory.
- No dynamic kernel code generation, patching or self-modification.
- Nonpaged NX-compatible allocations only.
- No user pointer is dereferenced directly.
- Every variable-size structure is validated for version, fixed header size, total size, bounds, integer overflow, generation, owner PID, client slot and CRC.
- Every object ID is bound to its creating process and generation.
- Shared-memory offsets are represented as bounded slot/length pairs, never trusted pointers.
- No socket, DNS, TLS, HTTP, credential or retry logic exists in the KMD.
- No kernel callback waits for network progress.
- Service loss must transition to cancellation, reset or device removal within a bounded deadline.

## Current transport evidence

- 256-entry bounded MPMC request ring.
- 16 process-owned client channels.
- 128-entry completion ring per client.
- 8 MiB fixed bulk arena with 128 leases.
- Per-process outstanding submission and bulk-byte quotas.
- Connection-generation rotation and stale-completion rejection.
- Asynchronous fence-value completions and out-of-order matching.
- Two simultaneously registered processes completed isolated owner-bound batches.

## Required pre-submission evidence

- Microsoft-signed SYS, DLL, service and catalog hashes.
- Public release symbols and private crash-analysis symbols.
- Final INF, CAT and installer/uninstaller hashes.
- Complete IOCTL, DDI and shared-memory ABI documentation.
- Threat model with data-flow diagrams and abuse cases.
- Driver Verifier logs with standard, code-integrity, low-resource and DMA-related checks as applicable.
- HLK results, including HVCI/readiness coverage.
- Secure Boot, VBS and Memory Integrity test matrix.
- Service termination, endpoint loss, delayed completion, cancellation, TDR and reset results.
- Protocol fuzzing corpus, crash triage and fixed-version evidence.
- Upgrade, rollback, disable and uninstall recovery evidence.
- Privacy statement for crash telemetry and endpoint metadata.
- Update signing, revocation and emergency-disable process.

## Anti-cheat review statements

The design does not inject code into a game, place replacement DLLs in a game directory, read another process's private memory, or place network credentials in kernel mode. Those properties reduce risk but do not imply compatibility or approval.

There is no universal anti-cheat approval. Status remains **unsupported** for each protected title until the relevant game and anti-cheat vendors have reviewed or verified the final Microsoft-signed package.

## Vendor inventory to disclose

- Display-class root-enumerated device hardware ID.
- Kernel service name and image path.
- UMD file names and registered D3D runtime entries.
- User-mode transport service name, account and privileges.
- Device objects, symbolic links, shared mappings and named events.
- Remote network protocols, destinations, ports and certificate policy.
- Installer, updater and uninstaller behavior.
- Crash-dump, log and telemetry locations.
- Loaded modules and expected process tree.

## Explicit non-capabilities

The production design must not expose facilities to inspect or modify unrelated processes, bypass process protections, conceal loaded components, spoof hardware identity, tamper with anti-cheat state, or silently fall back to an unapproved local proxy/injection path.

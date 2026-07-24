# RemoteGPU service and network endpoints

## Local UMD/service boundary — transport v2

- Shared mapping: `Local\RgpuPhase3TransportV2`
- Global request event: `Local\RgpuPhase3RequestV2`
- Per-client completion events: `Local\RgpuPhase3CompletionV2_<slot>`
- Control/request ring: 256 bounded MPMC entries
- Client channels: 16 atomically registered process-owned slots
- Completion ring per client: 128 entries
- Inline payload maximum: 128 bytes
- Shared bulk arena: 8 MiB
- Bulk leases: 128 fixed 64 KiB slots
- Maximum outstanding submissions per process: 128
- Maximum outstanding bulk bytes per process: 4 MiB

Every message carries a connection generation, sequence number, owner PID, client slot, object ID, fence value, size and CRC. Bulk leases carry the same owner/generation/sequence identity. Old-generation completions, cross-process objects, invalid leases and malformed offsets are rejected.

The normal UMD path uses bounded backpressure and multiple asynchronous batches. Full rings or quota exhaustion never overwrite memory and never cause unbounded allocation. Each process consumes only its dedicated completion ring, preventing one UMD process from stealing another process's completions.

The generic KMD broker and the DXGK scaffold contain no socket API, DNS, TLS, HTTP, credentials or blocking network waits. Networking remains an out-of-process service responsibility.

## Future remote service endpoint policy

The production service must own all remote networking and provide:

- one mutually authenticated TLS control endpoint;
- one authenticated ordered command/resource transport;
- one compressed frame-return transport;
- explicit heartbeat, cancellation, reset and device-loss timeouts;
- connection-generation rotation after service restart;
- certificate pinning or an enterprise trust policy;
- bounded retry and flow-control policy;
- no listener bound to a public interface unless explicitly configured;
- no hard-coded Colab hostname because notebook addresses are ephemeral.

No production remote URL or credential is committed to the repository. Endpoint material belongs in protected machine configuration and must not be stored in the INF, UMD, KMD, game directory or command payloads.

## Failure behavior

When the service, authenticated endpoint or remote executor is unavailable, adapter/device creation must fail or a defined device-removal/reset path must run. The implementation must not silently create a local hardware D3D12 device, reuse completions from an earlier service generation, or wait indefinitely in a kernel callback.

# RemoteGPU service and network endpoints

## Local driver/service boundary

- Shared mapping: `Local\RgpuPhase3QueueV1`
- Request event: `Local\RgpuPhase3RequestV1`
- Completion event: `Local\RgpuPhase3CompletionV1`
- Request slots: 16 fixed-size messages
- Completion slots: 16 fixed-size messages
- Maximum message payload: 192 bytes in the Phase 3 control-plane proof
- Full queues fail immediately; no overwrite and no unbounded allocation.

The UMD communicates only with the user-mode transport service through this bounded local boundary. The KMD broker contains no socket API, DNS, TLS, HTTP or blocking network waits.

## Remote service endpoint policy

The production service will own all remote networking. Its configuration must provide:

- one authenticated TLS control endpoint;
- one authenticated ordered command/resource transport;
- one compressed frame-return transport;
- explicit heartbeat and device-loss timeouts;
- certificate pinning or an enterprise trust policy;
- no listener bound to a public interface unless explicitly configured;
- no hard-coded Colab hostname because notebook addresses are ephemeral.

No production remote URL is committed to the repository. Runtime endpoint material belongs in protected machine configuration and must not be stored in the driver INF, UMD, KMD or game directory.

## Failure behavior

When the service, authenticated endpoint or remote executor is unavailable, adapter/device creation must fail or transition through a defined device-removal path. The implementation must not silently create a local hardware D3D12 device.

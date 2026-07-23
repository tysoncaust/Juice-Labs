# Tokyo Xtreme Racer / GPU-over-IP recovery plan

Status date: 2026-07-23

## Executive decision

The original goal contains two different products:

1. **Playable remote TXR:** run the complete Windows game on a Windows GPU host and stream video, audio, and input.
2. **CPU-only Windows client using a remote GPU as if it were local:** build a complete D3D12 virtualization runtime and replay the full graphics object/state model remotely.

The first product is now verified end to end with Sunshine and Moonlight. The second remains a substantial graphics-runtime project and is not completed by intercepting a few draw calls or copying frames at `Present`.

Google Colab is retained for bounded Vulkan, encoding, and protocol experiments. It is not the production host for TXR because the managed environment is Linux and ephemeral, while the closed-source game requires its Windows executable, D3D12/Agility runtime, Windows driver stack, persistent installation, and interactive session to remain together.

## Canonical production architecture

The final GPU-over-IP product is a **root-enumerated, render-only WDDM virtual adapter** on the bare-metal Windows client. The existing `d3d12.dll` proxy is now explicitly a Phase-1 research instrument used to discover TXR's actual D3D12 workload; it is not the production deployment mechanism.

The production frontend consists of a D3D12 UMD, a displayless render-only KMD and a user-mode transport service. The service communicates with a portable Linux/Vulkan executor using a versioned D3D12-semantic protocol. Network operations, authentication, compression and reconnection remain outside kernel callbacks. The adapter fails through a defined device-removal path rather than silently falling back to local hardware.

The complete design, difficult memory/address cases, acceptance gates and phased build order are in:

```text
rgpu/docs/REMOTE_WDDM_ARCHITECTURE.md
```

Sunshine/Moonlight remains the verified way to play immediately, but it is an operational fallback and validation path—not completion of the virtual GPU.

References:

- Google Colab FAQ and usage limits: https://research.google.com/colaboratory/faq.html
- Google Colab paid-service terms: https://research.google.com/colaboratory/tos_v5.html
- Sunshine documentation: https://docs.lizardbyte.dev/projects/sunshine/latest/

## What was wrong with the previous interception conclusion

Commit `cb8b089` restored the generic `ID3D12CoreModule` export table immediately after detecting the first **device**. Device creation occurs near the beginning of startup, so the claimed later queue/list observation window did not exist. The statement that every entry had been tested and none produced a queue/list was therefore unsupported.

The D3D11On12 conclusion also watched only `Draw` and `DrawIndexed`; it omitted instanced and indirect draws, compute dispatches, and deferred command-list execution.

Corrections on `fix/txr-export-table-probe`:

- the private table remains hooked after device creation;
- direct return values and changed out-parameters are inspected;
- total and per-entry call counts are logged;
- a timer restores the table even if it becomes idle;
- ten SDK-derived D3D11 work-submission methods are probed;
- the inline hooker now uses a near 5-byte relay and safely relocates short conditional branches;
- ordinary hooks, vtable-repatch survival, and short-Jcc relocation have dedicated passing tests;
- all four relevant device creation implementation bodies, including base `CreateCommandList`, now inline-hook successfully in live TXR;
- an interruption-safe TXR probe runner deploys, captures evidence, and restores the game directory.

## Verified D3D12 diagnostic evidence

### Controlled harness

`rgpu/proxy-d3d12/build_d3d12.ps1` builds and runs both acceptance tests.

Inline-hook acceptance:

```text
RESULT: inline hooking works, survives vtable re-patching, and relocates short Jcc
```

D3D12 command-tee acceptance:

```text
RESULT: D3D12 command stream captured (ExecuteCommandLists + barrier + clear + draw tee'd)
```

The harness captures one queue submission, one submitted list, one draw, two barriers, and one clear.

### TXR live probes

Corrected live probes repeatedly observed export-table entries `0`, `7`, `9`, `10`, `13`, and `14`. Entry `10` produced a device. The stable summary was:

```text
calls=21 devices=8 queues=0 lists=0 entryCalls=[0:4,7:1,9:4,10:4,13:4,14:4]
```

Latest live validation also logged:

```text
inline-hooked CreateCommandQueue impl
inline-hooked CreateCommandList impl
inline-hooked CreateCommandList1 impl
inline-hooked CreateCommandQueue1 impl
```

None of those D3D12 creation detours fired after installation. The final isolated
30-second validation did, however, prove that the D3D11On12 immediate context is active. It
recorded exactly 82 startup work calls:

```text
Dispatch=58
DrawIndexed=9
DrawIndexedInstanced=14
DrawInstanced=1
all other probed methods=0
```

The compute dimensions included `40960x1x1`, `128x128x1`, and `240x135x1`. This invalidates
Claude's conclusion that the D3D11 layer never submits work. Because the burst was concentrated
at startup and did not continue to the 100-call milestone, it is not yet proof that sustained
gameplay frames use this context exclusively.

### External D3D12 wrapper verification

RenderDoc 1.44 was launched against the same executable without the rgpu proxy. Its reliable
process log records:

```text
New D3D12 device created: nVidia / NVIDIA GeForce RTX 3050 Laptop GPU
Adding D3D12 frame capturer ... / <TXR swap-chain HWND>
Starting capture
Finished capture, Frame 2
Capture used DXIL
```

It also logged an unknown/private device-interface query for GUID
`{1f052807-0b46-4acc-8a89-364f793718a4}`. This proves a mature standard D3D12 wrapper can see
TXR's real device and frame boundary, so the claim that proprietary D3D12Core constructor
disassembly was the only route was wrong.

No replayable `.rdc` is claimed. RenderDoc logged `Stream created with invalid file handle`,
its thumbnail command reported that the container had no frame capture, and a later container
was corrupted. Invalid captures were deleted. The retained diagnostic summary is:

```text
rgpu/proxy-d3d12/test/evidence/renderdoc-txr-diagnostic-20260722.txt
```

The defensible conclusion is:

> The current rgpu probes have not acquired TXR's D3D12 queue/list stream, but they have
> acquired a real D3D11On12 startup compute/graphics stream, and RenderDoc proves a complete
> standard D3D12 wrapper can reach the actual device/frame boundary.

This directs the next phase toward full D3D11On12 object/state capture plus RenderDoc wrapper
comparison—not blind proprietary binary offsets. Evidence is stored under:

```text
rgpu/proxy-d3d12/test/evidence/
```

## Repeatable D3D12 diagnostic commands

```powershell
powershell -ExecutionPolicy Bypass -File .\rgpu\proxy-d3d12\build_d3d12.ps1

powershell -ExecutionPolicy Bypass -File .\rgpu\proxy-d3d12\tools\run_txr_probe.ps1 `
  -GameRoot "C:\Program Files\Tokyo Xtreme Racer" `
  -DurationSeconds 60
```

The live runner refuses invalid durations, removes only a stale copy of its own proxy, backs up an unrelated pre-existing `d3d12.dll`, bounds instrumentation, captures logs, stops the exact process it launched, and restores the game directory.

## Next virtualization milestone

1. Keep the inline-hook and D3D12 harness acceptance gates passing.
2. Expand the proven D3D11On12 context hook to capture state setters, resources/views,
   shaders, uploads/maps, clears/copies, queries, synchronization, and swap-chain `Present`.
3. Correlate work with repeated presented frames; a startup-only burst is not acceptance.
4. Compare rgpu with RenderDoc's open-source D3D12 wrapper at the observed private-interface,
   Agility acquisition, identity, child wrapping, swap-chain, and lifetime boundaries.
5. Require deterministic local replay before resuming remote handle translation and transport.
6. Keep the NVIDIA UMD result as negative evidence only: the observed adapter interface was
   major 11 and did not expose a D3D12 command-queue table. The speculative hook remains
   removed from production code.

Definition of done for capture:

- a real render queue is identified;
- `ExecuteCommandLists` or an equivalent documented submission boundary is observed repeatedly;
- submitted lists are associated with recorded operations;
- capture survives 10 minutes without a crash or material frame-rate regression;
- all hooks restore cleanly after exit.

Capturing the submission stream is still only the start. A remote D3D12 runtime additionally
needs object identities/lifetimes, resources/heaps, mapped uploads, descriptors, GPU virtual
addresses, root signatures, shaders/PSOs, command signatures, synchronization, queries,
residency, enhanced barriers, indirect arguments, and device-loss behavior.

## Verified playable production path

The Sunshine/Moonlight path was tested live on 2026-07-22:

- Sunshine service automatic and running on Windows host `Laptop`;
- paired MacBook successfully retrieved the application list;
- Moonlight initialized control, video, audio, and input streams;
- negotiated video stream: 1920x1080x60 H.264;
- actual encoder: Intel Quick Sync `h264_qsv`;
- client decode/render: Apple VideoToolbox and Metal on Apple M1;
- TXR remained alive for the full 25-second validation window while the stream was active;
- first video packet: 200 ms;
- network frame loss: 0.00%;
- average network latency: 3 ms;
- average host processing latency: 10.3 ms;
- average decode time: 4.22 ms;
- stereo audio initialized and received;
- bounded session shut down cleanly.

Evidence:

```text
rgpu/streaming/windows/evidence/moonlight-txr-e2e-20260722.txt
rgpu/streaming/windows/evidence/sunshine-live-session-20260722.log
```

Installed launchers:

```text
Windows: C:\Users\email\AppData\Local\Programs\Tokyo Xtreme Racer Remote\Tokyo Xtreme Racer - Remote.cmd
Mac:     /Users/macbook/Applications/Tokyo Xtreme Racer Remote.command
```

Operational flow:

1. Open the MacBook Applications folder and double-click the Remote launcher to start the Moonlight Desktop stream.
2. In the streamed Windows desktop, open Start and launch **Juice Labs > Tokyo Xtreme Racer Remote**.
3. Play normally.

A dedicated Sunshine tile is staged but not required. Applying it automatically was blocked because this MCP runs under the `power_user` profile rather than an administrator profile, and the Sunshine password SecretRef is locked. The existing Desktop application plus the two launchers is already tested and operational.

## Definition of done for true local-process GPU-over-IP

The separate D3D12 virtualization product is complete only when:

- the client game creates no local hardware graphics device;
- the remote renderer creates the accepted graphics device;
- first frame, menus, gameplay, resize, and shutdown work;
- uploads, descriptors, GPU VAs, fences, and device-loss tests pass;
- network loss stops rendering rather than silently falling back locally;
- a 30-minute run has bounded memory, no protocol divergence, and acceptable input-to-photon latency.

The current repository has not reached this definition. The Sunshine/Moonlight system has reached the playable remote-session definition.

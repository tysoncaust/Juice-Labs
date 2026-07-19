# Build + game-test notes (2026-07-20, tysoncaust fork)

## Build (this machine)
- Prereqs: **Go 1.26** (winget `GoLang.Go`) and, for `juicify` only, a **C compiler**
  for CGO (winget `BrechtSanders.WinLibs.POSIX.UCRT`, `CGO_ENABLED=1`).
- `go build -o bin/agent.exe ./cmd/agent/main.go` -> OK
- `go build -o bin/controller.exe ./cmd/controller/main.go` -> OK
- `go build -o bin/juicify.exe ./cmd/juicify/main.go` -> OK **only with CGO+gcc**
  (`juicify_windows.go` does `import "C"` and calls `GetJuiceVersion` from
  `juiceclient.dll`).

## What works
- `agent.exe` runs standalone, detects and serves the GPU, listens on :43210.
  Verified: **NVIDIA GeForce RTX 3050 Laptop GPU 4096MB, driver 610.74** is
  accepted — **no GPU refusal, no driver-incompatibility error, no crash**.
  (`Renderer_Win.exe --dump_gpus 0` also returns full GPU JSON, exit 0.)

## What is blocked (open-core limitation, NOT a bug/driver issue)
The open-source repo is orchestration only. The actual GPU interception is
proprietary and ships only with the commercial "Juice GPU" desktop:
- Agent GPU detection shells out to proprietary **`Renderer_Win.exe`** (present).
- `juicify` loads proprietary **`juiceclient.dll`** at startup and injects via
  proprietary **`launch.exe`**.
- On this machine `juiceclient.dll` is **MISSING** (only `RemoteGPUClient.dll`
  is present), and the commercial license is **expired**.

### Game test result
`juicify -address 127.0.0.1:43210 -- E:\Games\Subnautica\Subnautica.exe`
fails at init: `failed to get client version caused by GetLastError => 126`
(ERROR_MOD_NOT_FOUND = juiceclient.dll not found). The game never launches.
This happens before any game code runs, so Subnautica / TCG Card Shop Simulator
/ Tokyo Xtreme Racer all fail identically for the same reason.

**Conclusion:** the fork's server (agent) is fully functional and the current
GPU/driver is compatible, but you cannot game-test end-to-end from source alone —
the client injection engine (`juiceclient.dll`) is closed-source, absent here,
and license-gated. A real GPU-over-IP game test needs a licensed Juice client
runtime (their commercial product) or a different tool.

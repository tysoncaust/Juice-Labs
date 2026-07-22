# Build the rgpu d3d12 proxy (pass-through + command-stream tee) and its harness.
# Produces test\rgpu_d3d12.dll and test\rgpu_d3d12_harness.exe (runs the harness).
# Deploy: copy rgpu_d3d12.dll next to a D3D12 game's .exe as d3d12.dll (for TXR:
# ...\TokyoXtremeRacer\Binaries\Win64\d3d12.dll), launch with NO -d3d11, then read
# %TEMP%\rgpu_d3d12.log. Preserves the game's Agility SDK selection.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path "$here\test" | Out-Null
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }
Write-Host "g++: $gxx"
# Regenerate the COM-wrapper forwarders from the toolchain's own d3d12.h so vtable
# layout + aggregate-return ABI always match the compiler we build with.
$inc = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\x86_64-w64-mingw32\include\d3d12.h" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if ($py -and $inc) {
    & $py "$here\tools\gen_wrappers.py" $inc "$here\src\rgpu_d3d12_wrappers_gen.h"
    Write-Host "regenerated src\rgpu_d3d12_wrappers_gen.h"
} else { Write-Host "WARN: skipped wrapper codegen (python/d3d12.h not found); using committed header" }
# WIDL_EXPLICIT_AGGREGATE_RETURNS: make aggregate-return vtable slots use the hidden
# *__ret pointer form, matching the MSVC-built D3D12Core ABI exactly (see wrappers.h).
$cflags = "-DWIDL_EXPLICIT_AGGREGATE_RETURNS"

# The inline-hook relocation test is a required acceptance gate. It covers ordinary
# prologues, survival after a later vtable re-patch, and short conditional branches
# whose target lies inside the overwritten patch span (TXR CreateCommandList class).
& $gxx -std=c++17 -O2 "$here\test\rgpu_inlinehook_test.cpp" -o "$here\test\rgpu_inlinehook_test.exe"
Write-Host "built test\rgpu_inlinehook_test.exe; running..."
& "$here\test\rgpu_inlinehook_test.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# proxy DLL: main TU + the CINTERFACE slots TU (authoritative vtable indices)
& $gxx -std=c++17 -O2 $cflags -shared -static-libgcc -static-libstdc++ "-Wl,--kill-at" `
    "$here\src\rgpu_d3d12_proxy.cpp" "$here\src\rgpu_d3d12_slots.cpp" `
    -o "$here\test\rgpu_d3d12.dll" -ldxguid -ldxgi -lpsapi
Write-Host "built test\rgpu_d3d12.dll"
& $gxx -std=c++17 -O2 $cflags "$here\test\rgpu_d3d12_harness.cpp" -o "$here\test\rgpu_d3d12_harness.exe" -ldxguid
Write-Host "built test\rgpu_d3d12_harness.exe; running..."
Push-Location "$here\test"
try { & "$here\test\rgpu_d3d12_harness.exe"; $rc = $LASTEXITCODE } finally { Pop-Location }
exit $rc

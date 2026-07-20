# Build the rgpu d3d12 pass-through proxy (Phase-1, step 1). Produces
# test\rgpu_d3d12.dll. Deploy: copy it next to a D3D12 game's .exe as d3d12.dll
# (for TXR: ...\TokyoXtremeRacer\Binaries\Win64\d3d12.dll), launch the game with
# NO -d3d11, then read %TEMP%\rgpu_d3d12.log for ATTACH + D3D12CreateDevice.
# Preserves the game's Agility SDK selection (forwards to System32\d3d12.dll).
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path "$here\test" | Out-Null
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }
Write-Host "g++: $gxx"
& $gxx -std=c++17 -O2 -shared -static-libgcc -static-libstdc++ "-Wl,--kill-at" `
    "$here\src\rgpu_d3d12_proxy.cpp" -o "$here\test\rgpu_d3d12.dll"
Write-Host "built test\rgpu_d3d12.dll"

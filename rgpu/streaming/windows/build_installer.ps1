$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gxx = (Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\*WinLibs*\mingw64\bin\g++.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
if (-not $gxx) { $gxx = "g++" }
& $gxx -std=c++17 -O2 -static-libgcc -static-libstdc++ `
  "$here\install_sunshine_txr.cpp" -o "$here\install_sunshine_txr.exe" -ladvapi32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built $here\install_sunshine_txr.exe"

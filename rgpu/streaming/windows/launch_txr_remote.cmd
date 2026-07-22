@echo off
set "TXR_DIR=C:\Program Files\Tokyo Xtreme Racer\TokyoXtremeRacer\Binaries\Win64"
set "TXR_EXE=%TXR_DIR%\TokyoXtremeRacer-Win64-Shipping.exe"

if not exist "%TXR_EXE%" (
  echo Tokyo Xtreme Racer was not found:
  echo %TXR_EXE%
  pause
  exit /b 1
)

rem The rgpu diagnostic proxy must never be left deployed for normal streaming.
if exist "%TXR_DIR%\d3d12.dll" (
  echo Refusing to launch: a diagnostic d3d12.dll is still present in the game folder.
  echo Remove it or complete the bounded rgpu probe cleanup first.
  pause
  exit /b 2
)

start "Tokyo Xtreme Racer" /D "%TXR_DIR%" "%TXR_EXE%"

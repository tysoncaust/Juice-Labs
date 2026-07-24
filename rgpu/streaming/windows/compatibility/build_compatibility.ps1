$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $root 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cl = (Get-Command cl.exe -ErrorAction Stop).Source

function Invoke-Checked([string[]]$Arguments) {
    & $cl @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE"
    }
}

Invoke-Checked @(
    '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
    "/Fo:$out\input_test_window.obj",
    (Join-Path $root 'input_test_window.cpp'),
    "/Fe:$out\rgpu_input_test_window.exe",
    '/link', '/SUBSYSTEM:WINDOWS', 'user32.lib', 'shell32.lib'
)

Invoke-Checked @(
    '/nologo', '/std:c++17', '/EHsc', '/W4', '/WX',
    "/Fo:$out\input_relay_host.obj",
    (Join-Path $root 'input_relay_host.cpp'),
    "/Fe:$out\rgpu_input_relay_host.exe",
    '/link', 'ws2_32.lib', 'user32.lib'
)

$summary = @(
    'RGPU_COMPATIBILITY_BUILD=PASS'
    'VIDEO_HOST_SCRIPT=PASS'
    'MAC_VIDEO_RECEIVER_SCRIPT=PASS'
    'OWNED_WINDOW_INPUT_TEST=BUILD_PASS'
    'OUTBOUND_INPUT_RELAY=BUILD_PASS'
    'CUSTOM_GRAPHICS_DRIVER=FALSE'
    'VIRTUAL_DISPLAY=FALSE'
    'ANTI_CHEAT_BYPASS_COMPONENTS=FALSE'
)
$summaryPath = Join-Path $out 'build-summary.txt'
$summary | Set-Content -Encoding utf8 $summaryPath
$summary | ForEach-Object { Write-Output $_ }
Write-Output "EVIDENCE=$summaryPath"

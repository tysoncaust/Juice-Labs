$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$evidence = Join-Path $root 'evidence'

& (Join-Path $root 'build_compatibility.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Compatibility build failed' }

$videoPath = Join-Path $evidence 'video-host-latest.json'
$gamePath = Join-Path $evidence 'passive-destiny2-latest.json'
$macPath = Join-Path $evidence 'mac-video-receiver-latest.txt'
$inputPath = Join-Path $evidence 'remote-input-owned-window-latest.txt'
$guardPath = Join-Path $evidence 'destiny2-guard-latest.txt'
$topologyPath = Join-Path $evidence 'hybrid-gpu-topology-latest.txt'
$matrixPath = Join-Path $root 'compatibility-matrix.json'

foreach ($path in @($videoPath, $gamePath, $macPath, $inputPath, $guardPath, $topologyPath, $matrixPath)) {
    if (-not (Test-Path $path)) { throw "Required evidence missing: $path" }
}

$video = Get-Content $videoPath -Raw | ConvertFrom-Json
if ($video.passed -ne $true -or $video.encoder -ne 'h264_nvenc' -or
    $video.maximum_nvenc_utilization_percent -le 0 -or
    $video.custom_graphics_driver -ne $false -or
    $video.virtual_display -ne $false -or
    $video.process_injection -ne $false) {
    throw 'Bare-metal video evidence failed acceptance'
}

$game = Get-Content $gamePath -Raw | ConvertFrom-Json
if ($game.passed -ne $true -or
    $game.same_process_alive_after_capture -ne $true -or
    $game.passive_capture_passed -ne $true -or
    $game.anti_cheat -ne 'BattlEye' -or
    $game.battleye_service_status -ne 'Running' -or
    $game.remote_input_attempted -ne $false -or
    $game.process_memory_access_attempted -ne $false -or
    $game.process_injection_attempted -ne $false -or
    $game.anti_cheat_modification_attempted -ne $false) {
    throw 'Destiny 2 passive evidence failed acceptance'
}

$macText = Get-Content $macPath -Raw
$inputText = Get-Content $inputPath -Raw
$guardText = Get-Content $guardPath -Raw
$topologyText = Get-Content $topologyPath -Raw
if ($macText -notmatch 'MAC_LAN_VIDEO_RECEIVER=PASS') { throw 'Mac video decode evidence failed' }
if ($inputText -notmatch 'RGPU_REMOTE_INPUT_TEST=PASS' -or
    $inputText -notmatch 'PROTECTED_GAME_TARGETED=false') {
    throw 'Owned-window input evidence failed'
}
if ($guardText -notmatch 'GAME_GUARD_PROTECTED=true' -or
    $guardText -notmatch 'AUTOMATION_ALLOWED=false') {
    throw 'Protected-game guard evidence failed'
}
if ($topologyText -notmatch 'TOPOLOGY_INDEPENDENT_FFMPEG_GDIGRAB_NVENC=PASS') {
    throw 'Hybrid topology evidence failed'
}

$matrix = Get-Content $matrixPath -Raw | ConvertFrom-Json
if ($matrix.custom_driver_gate.proceed_with_custom_driver -ne $false -or
    $matrix.titles[0].stock_nvidia_nvenc_capture -ne 'pass' -or
    $matrix.titles[0].remote_mac_decode -ne 'pass' -or
    $matrix.titles[0].remote_keyboard_mouse_controller_in_game -ne 'not_tested_guard_refused') {
    throw 'Compatibility matrix decision gate failed'
}

$summary = @(
    'RGPU_BAREMETAL_COMPATIBILITY=PASS'
    'STOCK_NVIDIA_NVENC_VIDEO=PASS'
    'MAC_REMOTE_DECODE=PASS'
    'OWNED_WINDOW_REMOTE_INPUT=PASS'
    'DESTINY2_NORMAL_LAUNCH=PASS'
    'DESTINY2_BATTLEYE_REMAINED_RUNNING=PASS'
    'DESTINY2_PASSIVE_CAPTURE=PASS'
    'DESTINY2_REMOTE_INPUT=NOT_TESTED_GUARD_REFUSED'
    'DESTINY2_MATCHMAKING=NOT_TESTED'
    'CUSTOM_GRAPHICS_DRIVER_USED=FALSE'
    'VIRTUAL_GRAPHICS_ADAPTER_USED=FALSE'
    'ANTI_CHEAT_BYPASS_COMPONENTS=FALSE'
    'PROCEED_WITH_CUSTOM_DRIVER_FOR_PROTECTED_GAMES=FALSE'
    'FINAL_SUPPORTED_TITLE_STATUS=PROTOTYPE_ONLY_VENDOR_CONFIRMATION_PENDING'
)
$summaryPath = Join-Path $root 'compatibility-validation-latest.txt'
$summary | Set-Content -Encoding utf8 $summaryPath
$summary | ForEach-Object { Write-Output $_ }
Write-Output "EVIDENCE=$summaryPath"

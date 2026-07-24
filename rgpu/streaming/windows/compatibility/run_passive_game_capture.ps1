param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$GameProcessName,
    [string]$GameDisplayName = $GameProcessName,
    [ValidateRange(10, 300)]
    [int]$DurationSeconds = 30,
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot 'evidence')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null

$initial = @(Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue)
if ($initial.Count -eq 0) {
    Write-Output 'PASSIVE_GAME_CAPTURE=FAIL reason=game_process_not_running'
    exit 3
}
$initialPids = @($initial | Select-Object -ExpandProperty Id)

$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$nvidiaSmi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $EvidenceDirectory "passive-$GameProcessName-$stamp.log"
$stdoutPath = Join-Path $EvidenceDirectory "passive-$GameProcessName-$stamp.stdout.txt"
$dmonPath = Join-Path $EvidenceDirectory "passive-$GameProcessName-dmon-$stamp.txt"
$dmonErrorPath = Join-Path $EvidenceDirectory "passive-$GameProcessName-dmon-$stamp.err.txt"
$summaryPath = Join-Path $EvidenceDirectory "passive-$GameProcessName-$stamp.json"

$dmon = Start-Process -FilePath $nvidiaSmi -ArgumentList @(
    'dmon', '-s', 'u', '-d', '1', '-c', [string]($DurationSeconds + 4)
) -PassThru -RedirectStandardOutput $dmonPath -RedirectStandardError $dmonErrorPath

$arguments = @(
    '-hide_banner', '-loglevel', 'info',
    '-f', 'gdigrab', '-draw_mouse', '0', '-framerate', '30',
    '-video_size', '1920x1080', '-i', 'desktop',
    '-vf', 'scale=1280:720:flags=fast_bilinear',
    '-c:v', 'h264_nvenc', '-gpu', '0', '-preset', 'p1', '-tune', 'ull',
    '-rc', 'cbr', '-b:v', '8000k', '-maxrate', '8000k', '-bufsize', '800k',
    '-g', '30', '-bf', '0', '-zerolatency', '1', '-pix_fmt', 'yuv420p',
    '-t', [string]$DurationSeconds, '-an', '-f', 'null', 'NUL'
)

try {
    $capture = Start-Process -FilePath $ffmpeg -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $logPath
    if (-not $capture.WaitForExit(($DurationSeconds + 30) * 1000)) {
        Stop-Process -Id $capture.Id -Force -ErrorAction SilentlyContinue
        throw 'Passive capture exceeded bounded deadline'
    }
    $capture.Refresh()
    if (-not $dmon.WaitForExit(($DurationSeconds + 15) * 1000)) {
        Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    }

    $final = @(Get-Process -Name $GameProcessName -ErrorAction SilentlyContinue)
    $finalPids = @($final | Select-Object -ExpandProperty Id)
    $sameProcessAlive = @($initialPids | Where-Object { $finalPids -contains $_ }).Count -gt 0

    $battleEyeProcesses = @(
        Get-Process -Name 'BEService', 'BEService_x64', 'destiny2launcher' -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty ProcessName -Unique
    )
    $battleEyeService = Get-Service -Name 'BEService' -ErrorAction SilentlyContinue

    $logText = if (Test-Path $logPath) { Get-Content $logPath -Raw } else { '' }
    $dmonText = if (Test-Path $dmonPath) { Get-Content $dmonPath -Raw } else { '' }
    $frames = 0
    foreach ($match in [regex]::Matches($logText, 'frame=\s*(\d+)')) {
        $value = [int]$match.Groups[1].Value
        if ($value -gt $frames) { $frames = $value }
    }
    $maxNvenc = 0
    foreach ($line in ($dmonText -split "`r?`n")) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Count -ge 7 -and $parts[0] -eq '0' -and $parts[3] -match '^\d+$') {
            $value = [int]$parts[3]
            if ($value -gt $maxNvenc) { $maxNvenc = $value }
        }
    }

    $cleanCompletion = $logText -match 'Lsize=' -and
                       $logText -notmatch 'Conversion failed|Error writing trailer|Immediate exit requested'
    $reportedExitCode = $capture.ExitCode
    $capturePassed = $cleanCompletion -and
                     $logText -match 'h264_nvenc' -and
                     $frames -ge [Math]::Floor($DurationSeconds * 30 * 0.70) -and
                     $maxNvenc -gt 0
    $passed = $sameProcessAlive -and $capturePassed

    $summary = [ordered]@{
        schema = 'rgpu-passive-protected-game-test-v1'
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        game = $GameDisplayName
        game_process_name = $GameProcessName
        initial_pids = $initialPids
        final_pids = $finalPids
        same_process_alive_after_capture = $sameProcessAlive
        anti_cheat = 'BattlEye'
        observed_battleye_process_names = $battleEyeProcesses
        battleye_service_status = if ($battleEyeService) { [string]$battleEyeService.Status } else { 'not_observed' }
        passive_capture_passed = $capturePassed
        capture_sink = 'null_no_video_retained'
        capture = 'Windows GDI desktop capture'
        encoder = 'h264_nvenc'
        encoded_frames = $frames
        maximum_nvenc_utilization_percent = $maxNvenc
        ffmpeg_exit_code = $reportedExitCode
        ffmpeg_clean_completion_marker = $cleanCompletion
        remote_input_attempted = $false
        process_memory_access_attempted = $false
        process_injection_attempted = $false
        anti_cheat_modification_attempted = $false
        matchmaking_tested = $false
        vendor_support_confirmed = $false
        passed = $passed
        ffmpeg_log = $logPath
        nvidia_dmon = $dmonPath
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $summaryPath

    Write-Output "PASSIVE_GAME_CAPTURE=$(if ($passed) { 'PASS' } else { 'FAIL' })"
    Write-Output "GAME=$GameDisplayName"
    Write-Output "SAME_PROCESS_ALIVE=$($sameProcessAlive.ToString().ToLowerInvariant())"
    Write-Output "BATTLEYE_SERVICE=$(if ($battleEyeService) { $battleEyeService.Status } else { 'not_observed' })"
    Write-Output "ENCODED_FRAMES=$frames"
    Write-Output "MAX_NVENC_UTILIZATION_PERCENT=$maxNvenc"
    Write-Output 'VIDEO_RETAINED=false'
    Write-Output 'REMOTE_INPUT_ATTEMPTED=false'
    Write-Output "EVIDENCE=$summaryPath"

    if (-not $passed) { exit 2 }
}
finally {
    if ($dmon -and -not $dmon.HasExited) {
        Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    }
}

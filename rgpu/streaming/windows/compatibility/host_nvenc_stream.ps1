param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationAddress,
    [int]$Port = 50000,
    [ValidateRange(5, 3600)]
    [int]$DurationSeconds = 15,
    [ValidateRange(1, 240)]
    [int]$FramesPerSecond = 30,
    [ValidateRange(320, 7680)]
    [int]$Width = 1280,
    [ValidateRange(240, 4320)]
    [int]$Height = 720,
    [ValidateRange(500, 100000)]
    [int]$BitrateKbps = 8000,
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot 'evidence'),
    [switch]$DrawMouse
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null

$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$nvidiaSmi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
if (-not (Test-Path $nvidiaSmi)) { throw 'nvidia-smi.exe was not found' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $EvidenceDirectory "nvenc-sender-$stamp.log"
$stdoutPath = Join-Path $EvidenceDirectory "nvenc-sender-$stamp.stdout.txt"
$dmonPath = Join-Path $EvidenceDirectory "nvenc-dmon-$stamp.txt"
$dmonErrorPath = Join-Path $EvidenceDirectory "nvenc-dmon-$stamp.err.txt"
$summaryPath = Join-Path $EvidenceDirectory "nvenc-sender-$stamp.json"

$destination = "udp://$DestinationAddress`:${Port}?pkt_size=1316"
$bufferKbps = [Math]::Max(100, [int]($BitrateKbps / 10))
$drawMouseValue = if ($DrawMouse) { '1' } else { '0' }

$dmonCount = $DurationSeconds + 4
$dmon = Start-Process -FilePath $nvidiaSmi -ArgumentList @(
    'dmon', '-s', 'u', '-d', '1', '-c', [string]$dmonCount
) -PassThru -RedirectStandardOutput $dmonPath -RedirectStandardError $dmonErrorPath

$arguments = @(
    '-hide_banner', '-loglevel', 'info',
    '-f', 'gdigrab', '-draw_mouse', $drawMouseValue,
    '-framerate', [string]$FramesPerSecond,
    '-video_size', '1920x1080', '-i', 'desktop',
    '-vf', "scale=$Width`:$Height`:flags=fast_bilinear",
    '-c:v', 'h264_nvenc', '-gpu', '0', '-preset', 'p1', '-tune', 'ull',
    '-rc', 'cbr', '-b:v', "$BitrateKbps`k", '-maxrate', "$BitrateKbps`k",
    '-bufsize', "$bufferKbps`k", '-g', [string]$FramesPerSecond,
    '-bf', '0', '-zerolatency', '1', '-pix_fmt', 'yuv420p',
    '-t', [string]$DurationSeconds, '-an', '-f', 'mpegts', $destination
)

try {
    $sender = Start-Process -FilePath $ffmpeg -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $logPath
    if (-not $sender.WaitForExit(($DurationSeconds + 30) * 1000)) {
        Stop-Process -Id $sender.Id -Force -ErrorAction SilentlyContinue
        throw 'FFmpeg sender exceeded its bounded deadline'
    }
    $sender.Refresh()
    if (-not $dmon.WaitForExit(($DurationSeconds + 15) * 1000)) {
        Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    }

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

    $minimumFrames = [Math]::Floor($DurationSeconds * $FramesPerSecond * 0.75)
    $cleanCompletion = $logText -match 'Lsize=' -and
                       $logText -notmatch 'Conversion failed|Error writing trailer|Connection refused|Immediate exit requested'
    $reportedExitCode = $sender.ExitCode
    $passed = $cleanCompletion -and
              $logText -match 'h264_nvenc' -and
              $frames -ge $minimumFrames -and
              $maxNvenc -gt 0

    $summary = [ordered]@{
        schema = 'rgpu-baremetal-stream-v1'
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        passed = $passed
        destination = $destination
        capture = 'Windows GDI desktop capture'
        encoder = 'h264_nvenc'
        nvidia_gpu_index = 0
        requested_width = $Width
        requested_height = $Height
        requested_fps = $FramesPerSecond
        requested_bitrate_kbps = $BitrateKbps
        duration_seconds = $DurationSeconds
        encoded_frames = $frames
        maximum_nvenc_utilization_percent = $maxNvenc
        ffmpeg_exit_code = $reportedExitCode
        ffmpeg_clean_completion_marker = $cleanCompletion
        custom_graphics_driver = $false
        virtual_display = $false
        process_injection = $false
        input_injection = $false
        ffmpeg_log = $logPath
        nvidia_dmon = $dmonPath
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $summaryPath

    Write-Output "BAREMETAL_REMOTE_VIDEO=$(if ($passed) { 'PASS' } else { 'FAIL' })"
    Write-Output "DESTINATION=$destination"
    Write-Output 'CAPTURE=gdigrab'
    Write-Output 'ENCODER=h264_nvenc'
    Write-Output "FRAMES=$frames"
    Write-Output "MAX_NVENC_UTILIZATION_PERCENT=$maxNvenc"
    Write-Output "EVIDENCE=$summaryPath"

    if (-not $passed) { exit 2 }
}
finally {
    if ($dmon -and -not $dmon.HasExited) {
        Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    }
}

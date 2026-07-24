param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationAddress,
    [Parameter(Mandatory = $true)]
    [ValidateLength(10, 79)]
    [string]$Passphrase,
    [int]$Port = 51001,
    [ValidateRange(5, 3600)]
    [int]$DurationSeconds = 15,
    [ValidateRange(1, 120)]
    [int]$FramesPerSecond = 24,
    [ValidateRange(320, 3840)]
    [int]$Width = 1280,
    [ValidateRange(240, 2160)]
    [int]$Height = 720,
    [ValidateRange(500, 50000)]
    [int]$BitrateKbps = 8000,
    [ValidateRange(20, 2000)]
    [int]$SrtLatencyMs = 60,
    [ValidateSet('caller', 'listener')]
    [string]$TransportMode = 'caller',
    [ValidateRange(15, 100)]
    [int]$ConnectionWaitSeconds = 75,
    [string]$EvidenceDirectory = '',
    [switch]$GenerateTestTone,
    [switch]$DrawMouse
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $EvidenceDirectory = Join-Path $scriptRoot 'evidence'
}
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null

$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$nvidiaSmi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
$audioCapture = Join-Path $scriptRoot 'out\common\rgpu_wasapi_loopback_capture.exe'
$tonePlayer = Join-Path $scriptRoot 'out\common\rgpu_wasapi_tone_player.exe'
foreach ($required in @($nvidiaSmi, $audioCapture)) {
    if (-not (Test-Path $required)) { throw "Required component not found: $required" }
}
if ($GenerateTestTone -and -not (Test-Path $tonePlayer)) {
    throw "Tone player not found: $tonePlayer"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$sessionId = [guid]::NewGuid().ToString('N')
$pipeName = "\\.\pipe\rgpu-audio-$sessionId"
$audioInfoPath = Join-Path $EvidenceDirectory "srt-audio-ready-$stamp.json"
$audioEvidencePath = Join-Path $EvidenceDirectory "srt-audio-$stamp.json"
$audioStdoutPath = Join-Path $EvidenceDirectory "srt-audio-$stamp.stdout.txt"
$audioStderrPath = Join-Path $EvidenceDirectory "srt-audio-$stamp.stderr.txt"
$finalLogPath = Join-Path $EvidenceDirectory "srt-sender-$stamp.log"
$rawLogPath = Join-Path $env:TEMP "rgpu-srt-$sessionId.raw.log"
$rawStdoutPath = Join-Path $env:TEMP "rgpu-srt-$sessionId.stdout.txt"
$dmonPath = Join-Path $EvidenceDirectory "srt-nvenc-dmon-$stamp.txt"
$dmonErrorPath = Join-Path $EvidenceDirectory "srt-nvenc-dmon-$stamp.stderr.txt"
$summaryPath = Join-Path $EvidenceDirectory "srt-sender-$stamp.json"
$toneEvidencePath = Join-Path $EvidenceDirectory "srt-tone-$stamp.json"
$toneStdoutPath = Join-Path $EvidenceDirectory "srt-tone-$stamp.stdout.txt"
$toneStderrPath = Join-Path $EvidenceDirectory "srt-tone-$stamp.stderr.txt"

Remove-Item $audioInfoPath,$audioEvidencePath,$audioStdoutPath,$audioStderrPath,$rawLogPath,$rawStdoutPath,$dmonPath,$dmonErrorPath,$summaryPath,$toneEvidencePath,$toneStdoutPath,$toneStderrPath -Force -ErrorAction SilentlyContinue

$audioDuration = $DurationSeconds + 1
$audioProcess = Start-Process -FilePath $audioCapture -ArgumentList @(
    '--pipe', $pipeName,
    '--info', $audioInfoPath,
    '--evidence', $audioEvidencePath,
    '--duration', [string]$audioDuration,
    '--connect-timeout', '30'
) -PassThru -RedirectStandardOutput $audioStdoutPath -RedirectStandardError $audioStderrPath

$readyDeadline = (Get-Date).AddSeconds(20)
while (-not (Test-Path $audioInfoPath)) {
    if ($audioProcess.HasExited) {
        throw "WASAPI loopback source exited before becoming ready"
    }
    if ((Get-Date) -gt $readyDeadline) {
        Stop-Process -Id $audioProcess.Id -Force -ErrorAction SilentlyContinue
        throw 'Timed out waiting for the WASAPI loopback source'
    }
    Start-Sleep -Milliseconds 100
}
$audioInfo = Get-Content $audioInfoPath -Raw | ConvertFrom-Json
if (-not $audioInfo.ready) { throw 'WASAPI loopback source did not report ready=true' }

$escapedPassphrase = [Uri]::EscapeDataString($Passphrase)
$latencyUs = $SrtLatencyMs * 1000
$transportHost = if ($TransportMode -eq 'listener') { '0.0.0.0' } else { $DestinationAddress }
$destination = "srt://$transportHost`:${Port}?mode=$TransportMode&transtype=live&latency=$latencyUs&peerlatency=$latencyUs&rcvlatency=$latencyUs&passphrase=$escapedPassphrase&pbkeylen=32&tlpktdrop=1&linger=0"
$drawMouseValue = if ($DrawMouse) { '1' } else { '0' }
$bufferKbps = [Math]::Max(100, [int]($BitrateKbps / 8))
$gop = [Math]::Max(1, $FramesPerSecond)
$targetVideoFrames = $DurationSeconds * $FramesPerSecond

$dmonCount = $DurationSeconds + 8
$dmon = Start-Process -FilePath $nvidiaSmi -ArgumentList @(
    'dmon', '-s', 'u', '-d', '1', '-c', [string]$dmonCount
) -PassThru -RedirectStandardOutput $dmonPath -RedirectStandardError $dmonErrorPath

$arguments = @(
    '-hide_banner', '-loglevel', 'info',
    '-thread_queue_size', '1024',
    '-f', 'gdigrab', '-draw_mouse', $drawMouseValue,
    '-framerate', [string]$FramesPerSecond,
    '-video_size', '1920x1080', '-i', 'desktop',
    '-thread_queue_size', '1024',
    '-f', [string]$audioInfo.ffmpeg_sample_format,
    '-ar', [string]$audioInfo.sample_rate,
    '-ac', [string]$audioInfo.channels,
    '-i', $pipeName,
    '-map', '0:v:0', '-map', '1:a:0',
    '-vf', "setpts=PTS-STARTPTS,scale=$Width`:$Height`:flags=fast_bilinear",
    '-af', 'asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0',
    '-c:v', 'h264_nvenc', '-gpu', '0', '-preset', 'p1', '-tune', 'ull',
    '-rc', 'cbr', '-b:v', "$BitrateKbps`k", '-maxrate', "$BitrateKbps`k",
    '-bufsize', "$bufferKbps`k", '-g', [string]$gop,
    '-bf', '0', '-zerolatency', '1', '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-b:a', '160k', '-ar', '48000', '-ac', '2',
    '-mpegts_flags', '+resend_headers', '-muxdelay', '0', '-muxpreload', '0',
    '-fps_mode', 'cfr', '-frames:v', [string]$targetVideoFrames,
    '-f', 'mpegts', $destination
)

$senderStartedUtc = [DateTime]::UtcNow
$tone = $null
$sender = $null
try {
    $senderParams = @{ FilePath = $ffmpeg; ArgumentList = $arguments; PassThru = $true; RedirectStandardOutput = $rawStdoutPath; RedirectStandardError = $rawLogPath }
    $sender = Start-Process @senderParams

    if ($GenerateTestTone) {
        Start-Sleep -Milliseconds 700
        $toneDuration = [Math]::Max(2, $DurationSeconds - 2)
        $tone = Start-Process -FilePath $tonePlayer -ArgumentList @(
            '--duration', [string]$toneDuration,
            '--frequency', '997',
            '--amplitude', '0.03',
            '--evidence', $toneEvidencePath
        ) -PassThru -RedirectStandardOutput $toneStdoutPath -RedirectStandardError $toneStderrPath
    }

    if (-not $sender.WaitForExit(($DurationSeconds + $ConnectionWaitSeconds) * 1000)) {
        Stop-Process -Id $sender.Id -Force -ErrorAction SilentlyContinue
        throw 'Encrypted SRT sender exceeded its bounded deadline'
    }
    $sender.Refresh()

    if ($tone -and -not $tone.WaitForExit(($DurationSeconds + 10) * 1000)) {
        Stop-Process -Id $tone.Id -Force -ErrorAction SilentlyContinue
    }
    if (-not $audioProcess.WaitForExit(($DurationSeconds + 20) * 1000)) {
        Stop-Process -Id $audioProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if (-not $dmon.WaitForExit(($DurationSeconds + 15) * 1000)) {
        Stop-Process -Id $dmon.Id -Force -ErrorAction SilentlyContinue
    }

    $rawLog = if (Test-Path $rawLogPath) { Get-Content $rawLogPath -Raw } else { '' }
    $sanitizedLog = $rawLog.Replace($Passphrase, '<redacted>').Replace($escapedPassphrase, '<redacted>')
    $sanitizedLog | Set-Content -Encoding utf8 $finalLogPath
    Remove-Item $rawLogPath,$rawStdoutPath -Force -ErrorAction SilentlyContinue

    $dmonText = if (Test-Path $dmonPath) { Get-Content $dmonPath -Raw } else { '' }
    $audioEvidence = if (Test-Path $audioEvidencePath) {
        Get-Content $audioEvidencePath -Raw | ConvertFrom-Json
    } else { $null }

    $frames = 0
    foreach ($match in [regex]::Matches($sanitizedLog, 'frame=\s*(\d+)')) {
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

    $minimumFrames = [Math]::Floor($DurationSeconds * $FramesPerSecond * 0.60)
    $cleanCompletion = $sanitizedLog -match 'Lsize=' -and
        $sanitizedLog -notmatch 'Conversion failed|Error writing trailer|Connection refused|Immediate exit requested|Invalid argument'
    $audioPassed = $audioEvidence -and $audioEvidence.passed -and $audioEvidence.frames -gt 0
    $passed = $cleanCompletion -and
        $sanitizedLog -match 'h264_nvenc' -and
        $sanitizedLog -match 'Audio:\s*aac' -and
        $frames -ge $minimumFrames -and
        $audioPassed

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hashBytes = $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($Passphrase))
    } finally {
        $sha256.Dispose()
    }
    $passphraseHash = -join ($hashBytes | ForEach-Object { $_.ToString('x2') })
    $passphraseHash = $passphraseHash.Substring(0,12)

    $summary = [ordered]@{
        schema = 'rgpu-encrypted-srt-sender-v1'
        timestamp_utc = [DateTime]::UtcNow.ToString('o')
        sender_started_utc = $senderStartedUtc.ToString('o')
        passed = [bool]$passed
        destination_host = $DestinationAddress
        destination_port = $Port
        transport = 'SRT'
        transport_mode = $TransportMode
        reliable_transport = $true
        encryption = 'AES-256 via SRT pbkeylen=32'
        passphrase_sha256_12 = $passphraseHash
        srt_latency_ms = $SrtLatencyMs
        capture = 'Windows GDI desktop capture'
        audio_capture = 'WASAPI shared-mode default-render loopback'
        video_encoder = 'h264_nvenc'
        audio_encoder = 'aac'
        requested_width = $Width
        requested_height = $Height
        requested_fps = $FramesPerSecond
        requested_bitrate_kbps = $BitrateKbps
        duration_seconds = $DurationSeconds
        encoded_frames = $frames
        maximum_nvenc_utilization_percent = $maxNvenc
        nvenc_telemetry_observed = [bool]($maxNvenc -gt 0)
        audio_frames = if ($audioEvidence) { $audioEvidence.frames } else { 0 }
        audio_bytes = if ($audioEvidence) { $audioEvidence.bytes } else { 0 }
        continuity_silence_frames = if ($audioEvidence) { $audioEvidence.continuity_silence_frames } else { 0 }
        generated_test_tone = [bool]$GenerateTestTone
        ffmpeg_clean_completion_marker = [bool]$cleanCompletion
        ffmpeg_exit_code = $sender.ExitCode
        video_retained = $false
        custom_graphics_driver = $false
        virtual_display = $false
        process_injection = $false
        game_process_access = $false
        log = $finalLogPath
        audio_evidence = $audioEvidencePath
        nvidia_dmon = $dmonPath
    }
    $summary | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 $summaryPath

    Write-Output "ENCRYPTED_SRT_SENDER=$(if ($passed) { 'PASS' } else { 'FAIL' })"
    Write-Output 'TRANSPORT=SRT'
    Write-Output 'ENCRYPTION=AES-256'
    Write-Output 'RELIABLE_TRANSPORT=TRUE'
    Write-Output "MODE=$Width`x$Height@$FramesPerSecond"
    Write-Output "VIDEO_FRAMES=$frames"
    Write-Output "AUDIO_FRAMES=$(if ($audioEvidence) { $audioEvidence.frames } else { 0 })"
    Write-Output "MAX_NVENC_UTILIZATION_PERCENT=$maxNvenc"
    Write-Output "EVIDENCE=$summaryPath"
    if (-not $passed) { exit 2 }
}
finally {
    foreach ($process in @($sender, $tone, $audioProcess, $dmon)) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item $rawLogPath,$rawStdoutPath -Force -ErrorAction SilentlyContinue
}

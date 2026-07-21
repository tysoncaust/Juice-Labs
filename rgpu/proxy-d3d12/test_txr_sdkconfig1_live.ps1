$ErrorActionPreference = 'Stop'

$Repo = 'C:\Users\email\Documents\GitHub\Juice-Labs\rgpu\proxy-d3d12'
$Proxy = Join-Path $Repo 'test\rgpu_d3d12.dll'
$GameDir = 'C:\Program Files\Tokyo Xtreme Racer\TokyoXtremeRacer\Binaries\Win64'
$GameExe = Join-Path $GameDir 'TokyoXtremeRacer-Win64-Shipping.exe'
$Deployed = Join-Path $GameDir 'd3d12.dll'
$TempLog = Join-Path $env:TEMP 'rgpu_d3d12.log'
$SavedLog = Join-Path $Repo 'test\txr_sdkconfig1_rgpu_d3d12.log'
$ReportPath = Join-Path $Repo 'test\txr_sdkconfig1_live_test.json'
$Backup = $null
$Started = $null

function Get-TxrProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name='TokyoXtremeRacer-Win64-Shipping.exe'" -ErrorAction SilentlyContinue)
}

function Stop-TxrProcesses {
    foreach ($p in (Get-TxrProcesses)) {
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch {}
    }
    Start-Sleep -Seconds 2
}

if (-not (Test-Path -LiteralPath $Proxy)) { throw "Proxy not found: $Proxy" }
if (-not (Test-Path -LiteralPath $GameExe)) { throw "Game executable not found: $GameExe" }

$observations = [System.Collections.Generic.List[object]]::new()
$report = [ordered]@{
    started_utc = [DateTime]::UtcNow.ToString('o')
    game_exe = $GameExe
    proxy_sha256 = (Get-FileHash -LiteralPath $Proxy -Algorithm SHA256).Hash
    duration_seconds = 90
    launch_pid = $null
    observations = $observations
    log_markers = [ordered]@{}
    attach_count = 0
    device_create_count = 0
    process_pid_count = 0
    stable = $false
    capture_reached_real_factory_path = $false
    command_stream_live = $false
    cleanup_ok = $false
    error = $null
}

try {
    Stop-TxrProcesses

    if (Test-Path -LiteralPath $Deployed) {
        $Backup = "$Deployed.rgpu-backup-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss'))"
        Move-Item -LiteralPath $Deployed -Destination $Backup -Force
    }

    if (Test-Path -LiteralPath $TempLog) { Remove-Item -LiteralPath $TempLog -Force }
    if (Test-Path -LiteralPath $SavedLog) { Remove-Item -LiteralPath $SavedLog -Force }
    Copy-Item -LiteralPath $Proxy -Destination $Deployed -Force

    $Started = Start-Process -FilePath $GameExe -WorkingDirectory $GameDir `
        -ArgumentList @('-windowed', '-ResX=1280', '-ResY=720') -PassThru
    $report.launch_pid = $Started.Id

    $seenPids = [System.Collections.Generic.HashSet[int]]::new()
    for ($i = 0; $i -lt 18; $i++) {
        Start-Sleep -Seconds 5
        $ps = @(Get-TxrProcesses)
        foreach ($p in $ps) { [void]$seenPids.Add([int]$p.ProcessId) }
        $obs = [ordered]@{
            elapsed_seconds = (($i + 1) * 5)
            process_count = $ps.Count
            pids = @($ps | ForEach-Object { [int]$_.ProcessId })
            working_set_mb = [math]::Round((($ps | Measure-Object WorkingSetSize -Sum).Sum / 1MB), 1)
            log_bytes = if (Test-Path -LiteralPath $TempLog) { (Get-Item -LiteralPath $TempLog).Length } else { 0 }
        }
        $observations.Add([pscustomobject]$obs)
    }
    $report.process_pid_count = $seenPids.Count

    $log = if (Test-Path -LiteralPath $TempLog) { Get-Content -LiteralPath $TempLog -Raw } else { '' }
    if ($log) { [IO.File]::WriteAllText($SavedLog, $log, [Text.UTF8Encoding]::new($false)) }

    $markers = [ordered]@{
        attach = 'ATTACH host='
        sdk_config_hook = 'hooked ID3D12SDKConfiguration1::CreateDeviceFactory'
        sdk_create_factory_call = 'ID3D12SDKConfiguration1::CreateDeviceFactory sdk='
        device_factory_hook = 'hooked ID3D12DeviceFactory::CreateDevice'
        factory_create_device_call = 'ID3D12DeviceFactory::CreateDevice fl='
        armed_factory_device = 'armed DeviceFactory device'
        check_feature_support = 'game USES our hooked device'
        create_queue_base = 'game called CreateCommandQueue (base)'
        create_queue_agility = 'game called CreateCommandQueue1 (Agility)'
        create_list_base = 'game called CreateCommandList (base)'
        create_list_agility = 'game called CreateCommandList1 (Agility)'
        execute_command_lists = 'submit#'
        draw = 'FIRST Draw'
        dispatch = 'FIRST Dispatch'
        enhanced_barrier = 'FIRST Barrier (enhanced/GCL7)'
        execute_indirect = 'FIRST ExecuteIndirect'
        close = 'FIRST Close'
    }
    foreach ($key in $markers.Keys) {
        $needle = $markers[$key]
        $report.log_markers[$key] = ([regex]::Matches($log, [regex]::Escape($needle))).Count
    }
    $report.attach_count = $report.log_markers.attach
    $report.device_create_count = ([regex]::Matches($log, 'D3D12CreateDevice riid=')).Count
    $report.capture_reached_real_factory_path = (
        $report.log_markers.sdk_config_hook -gt 0 -and
        $report.log_markers.sdk_create_factory_call -gt 0 -and
        $report.log_markers.device_factory_hook -gt 0 -and
        $report.log_markers.factory_create_device_call -gt 0 -and
        $report.log_markers.armed_factory_device -gt 0
    )
    $report.command_stream_live = (
        $report.log_markers.execute_command_lists -gt 0 -or
        $report.log_markers.draw -gt 0 -or
        $report.log_markers.dispatch -gt 0 -or
        $report.log_markers.enhanced_barrier -gt 0 -or
        $report.log_markers.execute_indirect -gt 0 -or
        $report.log_markers.close -gt 0
    )

    # One attach and one observed PID are expected. Multiple distinct PIDs or repeated
    # attaches are evidence of a crash/relaunch cycle rather than normal attract mode.
    $aliveSamples = @($observations | Where-Object { $_.process_count -gt 0 }).Count
    $report.stable = ($aliveSamples -ge 12 -and $seenPids.Count -le 1 -and $report.attach_count -le 1)
}
catch {
    $report.error = $_.Exception.ToString()
}
finally {
    Stop-TxrProcesses
    try {
        if (Test-Path -LiteralPath $Deployed) { Remove-Item -LiteralPath $Deployed -Force }
        if ($Backup -and (Test-Path -LiteralPath $Backup)) {
            Move-Item -LiteralPath $Backup -Destination $Deployed -Force
        }
        $report.cleanup_ok = (-not (Test-Path -LiteralPath $Deployed)) -or ($Backup -ne $null)
    }
    catch {
        if (-not $report.error) { $report.error = $_.Exception.ToString() }
        $report.cleanup_ok = $false
    }
    $report.completed_utc = [DateTime]::UtcNow.ToString('o')
    $json = $report | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($ReportPath, $json, [Text.UTF8Encoding]::new($false))
    Write-Output $json
}

if ($report.error -or -not $report.cleanup_ok) { exit 1 }
exit 0

$ErrorActionPreference = 'Stop'
$out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'out\security-status.json'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null

$secureBoot = $null
$secureBootError = $null
try { $secureBoot = [bool](Confirm-SecureBootUEFI) } catch { $secureBootError = $_.Exception.Message }

$deviceGuard = $null
$deviceGuardError = $null
try {
    $deviceGuard = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop
} catch { $deviceGuardError = $_.Exception.Message }

$result = [ordered]@{
    collected_utc = [DateTime]::UtcNow.ToString('o')
    secure_boot_enabled = $secureBoot
    secure_boot_query_error = $secureBootError
    hvci_configured = if ($deviceGuard) { @($deviceGuard.SecurityServicesConfigured) -contains 2 } else { $null }
    hvci_running = if ($deviceGuard) { @($deviceGuard.SecurityServicesRunning) -contains 2 } else { $null }
    virtualization_based_security_status = if ($deviceGuard) { $deviceGuard.VirtualizationBasedSecurityStatus } else { $null }
    device_guard_query_error = $deviceGuardError
    test_signing_required_by_package = $false
    production_policy_pass = ($secureBoot -eq $true)
}
$result | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $out
$result | ConvertTo-Json -Depth 4

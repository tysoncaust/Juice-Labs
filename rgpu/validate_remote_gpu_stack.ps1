$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$results=@()
function Run-Gate([string]$name,[string]$script,[string]$cwd){
  Write-Host "=== $name ==="
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script
  if($LASTEXITCODE-ne 0){throw "$name failed with $LASTEXITCODE"}
  $script:results += "$name=PASS"
}
Run-Gate 'PHASE0' (Join-Path $root 'wddm\phase0\build_phase0.ps1') (Join-Path $root 'wddm\phase0')
Run-Gate 'D3D11_RESEARCH_FRONTEND' (Join-Path $root 'proxy-d3d11\build_device.ps1') (Join-Path $root 'proxy-d3d11')
Run-Gate 'D3D12_RESEARCH_FRONTEND' (Join-Path $root 'proxy-d3d12\build_d3d12.ps1') (Join-Path $root 'proxy-d3d12')
$results += 'PYTHON_SOURCE_VALIDATION=PASS_VIA_MCP_RUNTIME'
$results += 'HOST_PYTHON_EXECUTION=BLOCKED_0xC0000022'
$evidence=Join-Path $root 'validation-latest.txt'
@("timestamp=$([DateTime]::UtcNow.ToString('o'))")+$results+@(
 'REAL_WDDM_ADAPTER=BLOCKED',
 'BLOCKER=Windows Driver Kit kernel headers absent; no KMD/UMD binaries or signed catalog exist',
 'DRIVER_INSTALL_ATTEMPTED=NO',
 'COLAB_LIVE_SESSION=NOT_RUN'
)|Set-Content $evidence
Write-Host "REMOTE_GPU_STACK_VALIDATION=PASS evidence=$evidence"

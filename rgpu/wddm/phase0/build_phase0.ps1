param([switch]$Window)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$out=Join-Path $root 'out'
New-Item -ItemType Directory -Force $out|Out-Null
$wdkRoots=@("$env:ProgramFiles(x86)\Windows Kits\10\Include","$env:ProgramFiles\Windows Kits\10\Include")
$wdkPresent=$false
foreach($p in $wdkRoots){if(Test-Path $p){$km=Get-ChildItem $p -Directory -ErrorAction SilentlyContinue|Where-Object{Test-Path (Join-Path $_.FullName 'km\wdm.h')}|Select-Object -First 1;if($km){$wdkPresent=$true}}}
Write-Host "WDK_HEADERS_PRESENT=$wdkPresent"
$inf=Get-Content (Join-Path $root 'driver\RemoteGpuRoot.inf') -Raw
foreach($required in @('Root\RemoteGpuRender','Class=Display','RemoteGpuKmd.sys','RemoteGpuUmd.dll','PnpLockdown=1')){if(-not $inf.Contains($required)){throw "INF validation failed: $required"}}
Write-Host 'INF_STATIC_VALIDATION=PASS'
$src=Join-Path $root 'presentation\rgpu_present_proof.cpp'
$exe=Join-Path $out 'rgpu_present_proof.exe'
& cl.exe /nologo /std:c++20 /EHsc /W4 /WX /DUNICODE /D_UNICODE $src /Fe:$exe user32.lib gdi32.lib
if($LASTEXITCODE-ne 0){throw 'compile failed'}
& $exe
if($LASTEXITCODE-ne 0){throw 'selftest failed'}
if($Window){& $exe --window}
$evidence=Join-Path $out 'phase0-evidence.txt'
@("timestamp=$([DateTime]::UtcNow.ToString('o'))","wdk_headers_present=$wdkPresent","inf_static_validation=PASS","presentation_selftest=PASS","driver_installed=NO","reason=Unsigned placeholder driver package is never installed by Phase 0 build")|Set-Content $evidence
Write-Host "PHASE0_BUILD=PASS evidence=$evidence"

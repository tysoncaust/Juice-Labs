$src = Join-Path $env:TEMP 'rgpu_d3d12.log'
$dst = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'test\last_harness_rgpu_d3d12.log'
if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $dst -Force; Write-Host $dst } else { throw "Missing $src" }

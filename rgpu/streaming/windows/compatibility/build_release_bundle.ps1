param([string]$OutputDirectory = '')



$ErrorActionPreference='Stop'



$root=Split-Path -Parent $MyInvocation.MyCommand.Path



$streamRoot=Resolve-Path (Join-Path $root '..\..')



if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory=Join-Path $root 'release-out' }



if (Test-Path $OutputDirectory) { Remove-Item $OutputDirectory -Recurse -Force }



New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null



& (Join-Path $root 'build_compatibility.ps1') -Mode Passive



$common=Join-Path $root 'out\common'; $passive=Join-Path $root 'out\passive'



if (Test-Path (Join-Path $root 'out\interactive')) { throw 'Passive release refused: interactive output exists' }



$bundle=Join-Path $OutputDirectory 'rgpu-streaming-passive-prerelease'; New-Item -ItemType Directory -Force -Path $bundle | Out-Null



Copy-Item (Join-Path $common '*.exe') $bundle



Copy-Item (Join-Path $passive 'mode-summary.txt') $bundle



Copy-Item (Join-Path $streamRoot 'ARCHITECTURE.md'),(Join-Path $streamRoot 'THREAT_MODEL.md'),(Join-Path $streamRoot 'COMPATIBILITY.md'),(Join-Path $streamRoot 'SECURITY.md'),(Join-Path $streamRoot 'PRIVACY.md'),(Join-Path $streamRoot 'LICENSE.md'),(Join-Path $streamRoot 'NOTICE') $bundle



$files=Get-ChildItem $bundle -File | Sort-Object Name



$checksums=@(); foreach($f in $files){$h=(Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant();$checksums += "$h  $($f.Name)"}



$checksums | Set-Content -Encoding ascii (Join-Path $bundle 'SHA256SUMS')



$spdxFiles=@(); foreach($f in (Get-ChildItem $bundle -File | Sort-Object Name)){ $h=(Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); $spdxFiles += [ordered]@{SPDXID="SPDXRef-File-$($f.Name -replace '[^A-Za-z0-9.-]','-')";fileName=$f.Name;checksums=@([ordered]@{algorithm='SHA256';checksumValue=$h});licenseConcluded='NOASSERTION';copyrightText='NOASSERTION'} }



$sbom=[ordered]@{spdxVersion='SPDX-2.3';dataLicense='CC0-1.0';SPDXID='SPDXRef-DOCUMENT';name='rgpu-streaming-passive-prerelease';documentNamespace="https://example.invalid/rgpu/spdx/$([guid]::NewGuid())";creationInfo=[ordered]@{created=[DateTime]::UtcNow.ToString('o');creators=@('Tool: rgpu-build_release_bundle.ps1')};packages=@([ordered]@{name='rgpu-streaming-passive';SPDXID='SPDXRef-Package';downloadLocation='NOASSERTION';filesAnalyzed=$true;licenseConcluded='MIT';licenseDeclared='MIT';copyrightText='Copyright (c) 2023-2026 Juice Technologies, Inc.'});files=$spdxFiles;relationships=@($spdxFiles|ForEach-Object{[ordered]@{spdxElementId='SPDXRef-Package';relationshipType='CONTAINS';relatedSpdxElement=$_.SPDXID}})}



$sbom | ConvertTo-Json -Depth 10 | Set-Content -Encoding utf8 (Join-Path $bundle 'sbom.spdx.json')



$zip=Join-Path $OutputDirectory 'rgpu-streaming-passive-prerelease.zip'; Compress-Archive -Path (Join-Path $bundle '*') -DestinationPath $zip -Force



(Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()+"  "+(Split-Path $zip -Leaf) | Set-Content -Encoding ascii ($zip+'.sha256')



Write-Output 'PASSIVE_RELEASE_BUNDLE=PASS'; Write-Output 'INTERACTIVE_COMPONENTS_INCLUDED=FALSE'; Write-Output 'SPDX_SBOM=PASS'; Write-Output "ARTIFACT=$zip"

[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
foreach ($name in @('tools/downloads','third_party','reports','input','work')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $root $name) | Out-Null
}
$headers = @{ 'User-Agent' = 'Drakengard3-Research-Setup'; Accept = 'application/vnd.github+json' }
$records = @()
function Get-Archive($url, $name, $digest) {
    $path = Join-Path $root "tools/downloads/$name"
    if (!(Test-Path -LiteralPath $path)) {
        Invoke-WebRequest -Uri $url -Headers $headers -OutFile "$path.partial"
        Move-Item -LiteralPath "$path.partial" -Destination $path
    }
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($digest -and $digest -ne "sha256:$hash") { throw "Digest mismatch: $name" }
    return @{ path = $path; sha256 = $hash; url = $url }
}
# Sources are pinned to the resolved commit and retained locally; no upstream code is executed here.
foreach ($repo in @('sp00nznet/ps3recomp','clienthax/GhidraSPU')) {
    $name = ($repo -split '/')[-1]
    $dest = Join-Path $root "third_party/$name"
    if (Test-Path -LiteralPath $dest) { Write-Host "Keep existing $dest"; continue }
    $info = Invoke-RestMethod "https://api.github.com/repos/$repo/commits/HEAD" -Headers $headers
    $archive = Get-Archive "https://api.github.com/repos/$repo/zipball/$($info.sha)" "$name-$($info.sha).zip" $null
    $staging = Join-Path $root "tools/downloads/stage-$name"
    Expand-Archive -LiteralPath $archive.path -DestinationPath $staging
    $unpacked = @(Get-ChildItem -LiteralPath $staging -Directory)
    if ($unpacked.Count -ne 1) { throw "Unexpected archive layout: $name" }
    Move-Item -LiteralPath $unpacked[0].FullName -Destination $dest
    $records += @{ name=$name; repository=$repo; commit=$info.sha; archive=$archive }
}
$ghidraPath = Join-Path $root 'tools/ghidra'
if (!(Test-Path -LiteralPath $ghidraPath)) {
    $release = Invoke-RestMethod 'https://api.github.com/repos/NationalSecurityAgency/ghidra/releases/latest' -Headers $headers
    $asset = @($release.assets | Where-Object name -Match '^ghidra_.*_PUBLIC.*\.zip$')[0]
    if (!$asset) { throw 'No official Ghidra PUBLIC ZIP found' }
    $archive = Get-Archive $asset.browser_download_url $asset.name $asset.digest
    Expand-Archive -LiteralPath $archive.path -DestinationPath $ghidraPath
    $records += @{ name='ghidra'; version=$release.tag_name; archive=$archive; upstream_digest=$asset.digest }
}
# Extract LLVM's official NSIS distribution without executing its installer.
$sevenZip = 'C:\Program Files\7-Zip\7z.exe'
foreach ($spec in @(
    @{name='llvm'; repo='llvm/llvm-project'; release='tags/llvmorg-20.1.8'; pattern='^LLVM-20\.1\.8-win64\.exe$'},
    @{name='rpcs3'; repo='RPCS3/rpcs3-binaries-win'; release='latest'; pattern='win64_msvc\.7z$'}
)) {
    $dest = Join-Path $root "tools/$($spec.name)"
    $marker = Join-Path $dest '.ready'
    if (Test-Path -LiteralPath $marker) { continue }
    $release = Invoke-RestMethod "https://api.github.com/repos/$($spec.repo)/releases/$($spec.release)" -Headers $headers
    $asset = @($release.assets | Where-Object name -Match $spec.pattern)[0]
    if (!$asset) { throw "No archive for $($spec.name)" }
    $archive = Get-Archive $asset.browser_download_url $asset.name $asset.digest
    & $sevenZip x $archive.path "-o$dest" -y -bso0 -bsp0
    if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $($spec.name)" }
    $record = @{name=$spec.name; version=$release.tag_name; archive=$archive; upstream_digest=$asset.digest}
    $record | ConvertTo-Json -Depth 5 | Set-Content $marker -Encoding utf8
    $records += $record
}
New-Item -ItemType Directory -Force (Join-Path $root 'tools/rpcs3/log') | Out-Null
# Recover provenance for already-unpacked source archives after interrupted setup.
foreach ($name in @('ps3recomp','GhidraSPU')) {
    $zip = Get-ChildItem (Join-Path $root 'tools/downloads') -Filter "$name-*.zip" | Select-Object -First 1
    if ($zip) {
        $records += @{name=$name; commit=($zip.BaseName -replace "^$name-", ''); sha256=(Get-FileHash $zip.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}
# Append history instead of silently replacing previous provenance.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
@{ captured_utc=[DateTime]::UtcNow.ToString('o'); tools=$records } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $root "reports/bootstrap-$stamp.json") -Encoding utf8
Write-Host 'Portable tools downloaded. See reports/bootstrap-*.json for revisions and SHA-256.'

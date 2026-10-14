$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
$env:GRADLE_USER_HOME = Join-Path $root 'tools/gradle-cache'
$env:GHIDRA_INSTALL_DIR = (Get-ChildItem "$root/tools/ghidra" -Directory | Select-Object -First 1).FullName
if (!$env:GHIDRA_INSTALL_DIR) { throw 'Ghidra is not installed' }
Push-Location (Join-Path $root 'third_party/GhidraSPU')
try {
    & .\gradlew.bat --no-daemon buildExtension
    if ($LASTEXITCODE -ne 0) { throw 'SPU extension build failed' }
    $zip = Get-ChildItem dist -Filter '*.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (!$zip) { throw 'No built extension ZIP found' }
    $dest = Join-Path $env:GHIDRA_INSTALL_DIR 'Ghidra/Extensions'
    if (!(Test-Path (Join-Path $dest 'GhidraSPU'))) {
        Expand-Archive -LiteralPath $zip.FullName -DestinationPath $dest
    }
    Write-Host "Built extension: $($zip.FullName)"
} finally { Pop-Location }

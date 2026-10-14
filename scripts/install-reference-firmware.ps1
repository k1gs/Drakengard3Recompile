$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
# Official Sony link from the PS3 system software support page, observed 2026-10-14.
$url = 'http://dus01.ps3.update.playstation.net/update/ps3/image/us/2026_0318_a2b60b6ac1d2e49e230144345616927c/PS3UPDAT.PUP'
$pup = Join-Path $root 'tools/downloads/PS3UPDAT-4.93.PUP'
if (!(Test-Path $pup)) {
    Invoke-WebRequest $url -OutFile "$pup.partial" -TimeoutSec 180
    Move-Item -LiteralPath "$pup.partial" -Destination $pup
}
if ((Get-Item $pup).Length -lt 100MB) { throw 'Unexpectedly small firmware file' }
$md5 = (Get-FileHash $pup -Algorithm MD5).Hash.ToLowerInvariant()
if ($md5 -ne 'a2b60b6ac1d2e49e230144345616927c') { throw 'Firmware differs from the digest in the official Sony download URL' }
$hash = (Get-FileHash $pup -Algorithm SHA256).Hash.ToLowerInvariant()
@{url=$url; sha256=$hash; size=(Get-Item $pup).Length; captured_utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $root 'reports/firmware-source-001.json')
New-Item -ItemType Directory -Force (Join-Path $root 'tools/rpcs3/log') | Out-Null
$process = Start-Process -FilePath (Join-Path $root 'tools/rpcs3/rpcs3.exe') -ArgumentList @('--headless','--installfw',('"'+$pup+'"')) -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput (Join-Path $root 'reports/firmware-install-001.txt') -RedirectStandardError (Join-Path $root 'reports/firmware-install-errors-001.txt')
Copy-Item (Join-Path $root 'tools/rpcs3/log/RPCS3.log') (Join-Path $root 'reports/firmware-install-001.log')
if ($process.ExitCode -ne 0) { throw "Firmware install exit $($process.ExitCode)" }
Write-Host 'RPCS3 firmware installer finished. Verify the installation log before booting.'

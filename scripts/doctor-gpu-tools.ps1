[CmdletBinding()]
param([switch]$Json)
$ErrorActionPreference='Stop'
$candidates=@()
foreach($base in @($env:ProgramFiles,${env:ProgramFiles(x86)})) {
    if(!$base){continue}
    $pixRoot=Join-Path $base 'Microsoft PIX'
    if(Test-Path -LiteralPath $pixRoot) {
        foreach($dir in Get-ChildItem -LiteralPath $pixRoot -Directory) {
            $tool=Join-Path $dir.FullName 'pixtool.exe'
            $dll=Join-Path $dir.FullName 'WinPixGpuCapturer.dll'
            if(Test-Path -LiteralPath $tool) {
                $candidates+=@{path=$tool;version=(Get-Item -LiteralPath $tool).VersionInfo.FileVersion;
                    gpu_capturer=if(Test-Path -LiteralPath $dll){$dll}else{$null}}
            }
        }
    }
}
$renderdoc=Get-Command renderdoccmd.exe -ErrorAction SilentlyContinue
$rd=if($renderdoc){$renderdoc.Source}else{$null}
if(!$rd -and $env:ProgramFiles) {
    $path=Join-Path $env:ProgramFiles 'RenderDoc/renderdoccmd.exe'
    if(Test-Path -LiteralPath $path){$rd=$path}
}
$developerMode=$null
try {
    $unlock=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows/CurrentVersion/AppModelUnlock' -ErrorAction Stop
    $developerMode=($unlock.AllowDevelopmentWithoutDevLicense -eq 1)
} catch { }
$result=[ordered]@{pix=@($candidates);renderdoc=$rd;developer_mode=$developerMode;
    capture_verified=$false;note='Discovery only; installation does not prove capture or replay.'}
if($Json){$result | ConvertTo-Json -Depth 5}else{[pscustomobject]$result}

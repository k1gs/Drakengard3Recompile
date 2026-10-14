$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = 'C:\Program Files\Java\jdk-21'
$launcher = Get-ChildItem "$root\tools\ghidra\*\ghidraRun.bat" | Select-Object -First 1
if (!$launcher) { throw 'Run scripts/bootstrap.ps1 first' }
& $launcher.FullName

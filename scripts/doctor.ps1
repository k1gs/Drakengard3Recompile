$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$candidates = [ordered]@{
    Python = (Get-Command python -ErrorAction SilentlyContinue).Source
    Git = (Get-Command git -ErrorAction SilentlyContinue).Source
    CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    ClangCL = "$root\tools\llvm\bin\clang-cl.exe"
    Ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    Java21 = 'C:\Program Files\Java\jdk-21\bin\java.exe'
    SevenZip = 'C:\Program Files\7-Zip\7z.exe'
    Ghidra = (Get-ChildItem "$root\tools\ghidra\*\ghidraRun.bat" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    PS3Recomp = "$root\third_party\ps3recomp\CMakeLists.txt"
    RPCS3 = "$root\tools\rpcs3\rpcs3.exe"
}
$candidates.GetEnumerator() | ForEach-Object {
    [pscustomobject]@{ Tool=$_.Key; Available=([bool]$_.Value -and (Test-Path -LiteralPath $_.Value)); Path=$_.Value }
} | Format-Table -AutoSize

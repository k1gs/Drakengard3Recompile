@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "PATH=%ROOT%\tools\llvm\bin;%PATH%"
set CI=1
pushd "%ROOT%\third_party\ps3recomp"
for %%T in (test_ppu_lift.py test_ppu_branches.py test_spu_disasm_ri10.py test_lifter_header_name.py) do (
    "%ROOT%\.venv\Scripts\python.exe" "tools\%%T"
    if errorlevel 1 goto failed
)
popd
exit /b 0
:failed
popd
exit /b 1

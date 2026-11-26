@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "PATH=%ROOT%\tools\llvm\bin;%PATH%"
cmake --build "%ROOT%\build\game" --target edat_contract
exit /b %errorlevel%

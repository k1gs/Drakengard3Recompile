@echo off
setlocal
set "ROOT=%~dp0.."
set "PROBE=%ROOT%\work\startup-probe-005"
if not "%~1"=="" set "PROBE=%~f1"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
rem /EHs permits our diagnostic exceptions to cross extern-C helper boundaries.
"%ROOT%\tools\llvm\bin\clang-cl.exe" /nologo /std:c++20 /EHs /W3 /O2 /bigobj /I"%PROBE%" "%PROBE%\ppu_recomp.cpp" "%PROBE%\frontier.cpp" "%ROOT%\native\startup_probe.cpp" /Fe:"%PROBE%\startup_probe.exe" /Fo"%PROBE%\\" /link bcrypt.lib
exit /b %errorlevel%

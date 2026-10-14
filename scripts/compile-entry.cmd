@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
"%ROOT%\tools\llvm\bin\clang-cl.exe" /nologo /std:c++20 /W0 /bigobj /c "%ROOT%\work\entry-lift-001\ppu_recomp_000.cpp" /Fo"%ROOT%\work\entry-lift-001\entry.obj"
exit /b %errorlevel%

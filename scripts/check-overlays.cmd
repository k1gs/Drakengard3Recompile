@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
"%ROOT%\tools\llvm\bin\clang-cl.exe" /nologo /O2 /EHsc /std:c++20 "%ROOT%\tests\native_overlay_contract.cpp" "/Fo%ROOT%\build\game\overlay_contract.obj" "/Fe%ROOT%\build\game\overlay_contract.exe"
exit /b %errorlevel%

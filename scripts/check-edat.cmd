@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
"%ROOT%\tools\llvm\bin\clang-cl.exe" /nologo /TC /O2 /std:c17 /D_CRT_SECURE_NO_WARNINGS "/I%ROOT%\build\game" "%ROOT%\tests\native_edat_contract.c" "/Fo%ROOT%\build\game\edat_contract.obj" "/Fe%ROOT%\build\game\edat_contract.exe"
exit /b %errorlevel%

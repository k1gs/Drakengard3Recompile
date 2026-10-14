@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "PATH=%ROOT%\tools\llvm\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "%ROOT%\third_party\ps3recomp" -B "%ROOT%\build\runtime" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS3RECOMP_BUILD_TESTS=ON
if errorlevel 1 exit /b %errorlevel%
cmake --build "%ROOT%\build\runtime" --parallel 4
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir "%ROOT%\build\runtime" --output-on-failure
exit /b %errorlevel%

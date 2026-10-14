@echo off
setlocal
set "ROOT=%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
set "PATH=%ROOT%\tools\llvm\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "%ROOT%\port" -B "%ROOT%\build\game" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O1 /Z7 /DNDEBUG" "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O1 /Z7 /DNDEBUG" "-DPython3_EXECUTABLE=%ROOT%\.venv\Scripts\python.exe" "-DSPU_DIR=%ROOT%\work\full-spu-001" -DPS3RECOMP_BUILD_TESTS=OFF
if errorlevel 1 exit /b %errorlevel%
cmake --build "%ROOT%\build\game" --target DrakengardRecompile --parallel 4
exit /b %errorlevel%

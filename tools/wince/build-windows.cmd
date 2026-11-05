@echo off
rem Development-only WinCE console and runtime tests, using this checkout's paths.
rem UTF-8 is needed for Ninja to parse the French MSVC /showIncludes prefix correctly.
chcp 65001 >nul
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=%ProgramFiles%\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%"
cd /d "%~dp0..\.."
if errorlevel 1 exit /b 1
cmake -S . -B build-wince-verified -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TRY_COMPILE_CONFIGURATION=Release ^
  -DDREAM_RENDERER=OFF -DDREAM_DEV_INTERPRETER=ON -DDREAM_BUILD_TESTS=ON -DDREAM_WERROR=OFF
if errorlevel 1 exit /b 1
cmake --build build-wince-verified --target dream-wince dream_runtime_tests --parallel 6
if errorlevel 1 exit /b 1
ctest --test-dir build-wince-verified -R "^(runtime.unit|wince\..*)$" --output-on-failure
exit /b %errorlevel%

@echo off
REM Build coronet with MSVC 2022
call "D:\dev\tools\VisualStudio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\dev\workspace\yidaoyun\coronet

echo === Configuring coronet (Release) ===
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo.
echo === Building ===
cmake --build build-windows --config Release
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo.
echo === Build complete ===
dir build-windows\bench\*.exe 2>nul
dir build-windows\test\*.exe 2>nul

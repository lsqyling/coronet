@echo off
REM coronet build & test script (Windows)
REM Run from a Visual Studio Developer Command Prompt (x64)

set BUILD_DIR=build
set BUILD_TYPE=Debug

cmake -S . -B %BUILD_DIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCORONET_BUILD_TESTS=ON ^
    -DCORONET_BUILD_EXAMPLES=OFF ^
    -DCORONET_USE_MIMALLOC=OFF
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cmake --build %BUILD_DIR%
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cd %BUILD_DIR%
ctest --output-on-failure
cd ..

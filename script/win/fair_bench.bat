@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM coronet vs ASIO Fair Benchmark (Windows)
REM Usage: script\windows\fair_bench.bat
REM ============================================================

set BUILD_DIR=build-windows
set CORO_BIN=%BUILD_DIR%\bench\redis_echo_coro.exe
set ASIO_BIN=%BUILD_DIR%\bench\redis_echo_asio.exe
set LOADGEN=%BUILD_DIR%\bench\redis_loadgen.exe
set DATA_DIR=data

set TIMESTAMP=%date:~0,4%%date:~5,2%%date:~8,2%_%time:~0,2%%time:~3,2%%time:~6,2%
set TIMESTAMP=%TIMESTAMP: =0%
set RESULT_DIR=%DATA_DIR%\win_bench_%TIMESTAMP%
mkdir "%RESULT_DIR%" 2>nul

set CONCURRENCIES=10 50 100 200 500 1000
set TOTAL_REQUESTS=100000
set CORO_PORT=7600
set ASIO_PORT=8600

echo ============================================
echo   coronet vs ASIO - Windows Fair Benchmark
echo ============================================
echo Timestamp:  %TIMESTAMP%
echo Total reqs: %TOTAL_REQUESTS%
echo Concurrency: %CONCURRENCIES%
echo Results:    %RESULT_DIR%
echo.

echo concurrency,coronet,asio > "%RESULT_DIR%\results.csv"

for %%c in (%CONCURRENCIES%) do (
    echo --- Concurrency: %%c ---

    :: coronet
    taskkill /f /im redis_echo_coro.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
    start /b "" "%CORO_BIN%" %CORO_PORT% >nul 2>&1
    timeout /t 2 /nobreak >nul
    set RPS=0
    for /f "tokens=2 delims=:" %%a in ('"%LOADGEN%" -c %%c -n %TOTAL_REQUESTS% -p %CORO_PORT% -h 127.0.0.1 2^>^&1 ^| findstr /i "RPS:"') do set RPS=%%a
    if "!RPS!"=="" set RPS=0
    echo   coronet: !RPS! req/s
    taskkill /f /im redis_echo_coro.exe >nul 2>&1
    timeout /t 1 /nobreak >nul

    :: ASIO
    taskkill /f /im redis_echo_asio.exe >nul 2>&1
    timeout /t 1 /nobreak >nul
    start /b "" "%ASIO_BIN%" %ASIO_PORT% >nul 2>&1
    timeout /t 2 /nobreak >nul
    set ARPS=0
    for /f "tokens=2 delims=:" %%a in ('"%LOADGEN%" -c %%c -n %TOTAL_REQUESTS% -p %ASIO_PORT% -h 127.0.0.1 2^>^&1 ^| findstr /i "RPS:"') do set ARPS=%%a
    if "!ARPS!"=="" set ARPS=0
    echo   ASIO:    !ARPS! req/s
    taskkill /f /im redis_echo_asio.exe >nul 2>&1
    timeout /t 1 /nobreak >nul

    echo %%c,!RPS!,!ARPS! >> "%RESULT_DIR%\results.csv"
    echo.
)

echo ============================================
echo   Results Summary
echo ============================================
echo Results: %RESULT_DIR%\results.csv
type "%RESULT_DIR%\results.csv"

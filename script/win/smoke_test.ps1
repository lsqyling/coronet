# ============================================================
# Quick smoke test — verify both servers work on Windows
# Usage: powershell -File script/windows/smoke_test.ps1
# ============================================================
param(
    [string]$BuildDir = "build-windows",
    [int]$CoroPort = 7701,
    [int]$AsioPort = 7702
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjDir = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$BuildPath = Join-Path $ProjDir $BuildDir

$CoroBin = Join-Path $BuildPath "bench\redis_echo_coro.exe"
$AsioBin = Join-Path $BuildPath "bench\redis_echo_asio.exe"
$LoadGen = Join-Path $BuildPath "bench\redis_loadgen.exe"

Write-Host "=== coronet Windows Smoke Test ===" -ForegroundColor Cyan

# ---- coronet ----
Write-Host "--- coronet (coroutine) port=$CoroPort ---" -ForegroundColor Yellow
$coroJob = Start-Job -ScriptBlock {
    param($bin, $port)
    & $bin $port 2>&1 | Out-Null
} -ArgumentList $CoroBin, $CoroPort
Start-Sleep -Seconds 2

if ($coroJob.State -eq "Running") {
    & $LoadGen -c 10 -n 200 -p $CoroPort -h 127.0.0.1
} else {
    Write-Host "FAILED to start coronet server" -ForegroundColor Red
}
Stop-Job $coroJob -ErrorAction SilentlyContinue
Remove-Job $coroJob -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# ---- ASIO ----
Write-Host "--- ASIO (callback) port=$AsioPort ---" -ForegroundColor Yellow
$asioJob = Start-Job -ScriptBlock {
    param($bin, $port)
    & $bin $port 2>&1 | Out-Null
} -ArgumentList $AsioBin, $AsioPort
Start-Sleep -Seconds 2

if ($asioJob.State -eq "Running") {
    & $LoadGen -c 10 -n 200 -p $AsioPort -h 127.0.0.1
} else {
    Write-Host "FAILED to start ASIO server" -ForegroundColor Red
}
Stop-Job $asioJob -ErrorAction SilentlyContinue
Remove-Job $asioJob -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== Smoke test complete ===" -ForegroundColor Green

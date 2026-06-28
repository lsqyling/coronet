# ============================================================
# Scaling test — quick increasing-concurrency check
# Usage: powershell -File script/windows/scaling_test.ps1
# ============================================================
param(
    [string]$BuildDir = "build-windows",
    [int]$CoroutinePort = 7800,
    [int]$AsioPort = 8800
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjDir = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$BuildPath = Join-Path $ProjDir $BuildDir

$CoroBin = Join-Path $BuildPath "bench\redis_echo_coro.exe"
$AsioBin = Join-Path $BuildPath "bench\redis_echo_asio.exe"
$LoadGen = Join-Path $BuildPath "bench\redis_loadgen.exe"

$levels = @(
    @{c=10;  n=10000},
    @{c=50;  n=50000},
    @{c=100; n=100000},
    @{c=200; n=100000},
    @{c=500; n=100000}
)

Write-Host "=== coronet Windows Scaling Test ===" -ForegroundColor Cyan
Write-Host ""

foreach ($level in $levels) {
    $conc = $level.c
    $reqs = $level.n
    Write-Host "--- c=$conc n=$reqs ---" -ForegroundColor Yellow

    # coronet
    $procName = "redis_echo_coro"
    Get-Process -Name $procName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $job = Start-Job -ScriptBlock { param($s, $p) & $s $p 2>&1 | Out-Null } -ArgumentList $CoroBin, $CoroutinePort
    Start-Sleep -Seconds 2
    if ($job.State -eq "Running") {
        $output = & $LoadGen -c $conc -n $reqs -p $CoroutinePort -h 127.0.0.1 2>&1
        $rps = "0"
        if ($output -match 'RPS:\s*([\d.]+)') { $rps = $Matches[1] }
        Write-Host "  coronet: $rps req/s"
    } else {
        Write-Host "  coronet: CRASHED" -ForegroundColor Red
    }
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    # ASIO
    $procName = "redis_echo_asio"
    Get-Process -Name $procName -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    $job = Start-Job -ScriptBlock { param($s, $p) & $s $p 2>&1 | Out-Null } -ArgumentList $AsioBin, $AsioPort
    Start-Sleep -Seconds 2
    if ($job.State -eq "Running") {
        $output = & $LoadGen -c $conc -n $reqs -p $AsioPort -h 127.0.0.1 2>&1
        $rps = "0"
        if ($output -match 'RPS:\s*([\d.]+)') { $rps = $Matches[1] }
        Write-Host "  ASIO:    $rps req/s"
    } else {
        Write-Host "  ASIO:    CRASHED" -ForegroundColor Red
    }
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    Write-Host ""
}

Write-Host "=== Done ===" -ForegroundColor Green

# ============================================================
# coronet vs ASIO — Fair Redis PING Benchmark (Windows)
# Fresh server instance for EACH concurrency level.
# Usage: powershell -File script/windows/fair_bench.ps1
# ============================================================
param(
    [string]$BuildDir = "build-windows",
    [int[]]$Concurrencies = @(10, 50, 100, 200, 500, 1000),
    [int]$TotalRequests = 100000,
    [int]$CoroPort = 7600,
    [int]$AsioPort = 8600,
    [int]$TimeoutSec = 120
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjDir = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$BuildPath = Join-Path $ProjDir $BuildDir
$DataDir = Join-Path $ProjDir "data"

$CoroBin = Join-Path $BuildPath "bench\redis_echo_coro.exe"
$AsioBin = Join-Path $BuildPath "bench\redis_echo_asio.exe"
$LoadGen = Join-Path $BuildPath "bench\redis_loadgen.exe"

$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ResultDir = Join-Path $DataDir "win_bench_$Timestamp"
New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  coronet vs ASIO — Windows Fair Benchmark" -ForegroundColor Cyan
Write-Host "  (fresh server for each concurrency level)" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Timestamp:  $Timestamp"
Write-Host "Total reqs: $TotalRequests"
Write-Host "Concurrency: $Concurrencies"
Write-Host "Results:    $ResultDir"
Write-Host ""

# ---- Run single test ----
function Run-OneTest {
    param(
        [string]$Server,
        [string]$Name,
        [int]$Port,
        [int]$Conc
    )

    # Kill stale processes
    $procName = [System.IO.Path]::GetFileName($Server)
    Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($procName)) -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    # Start server
    $job = Start-Job -ScriptBlock {
        param($s, $p)
        & $s $p 2>&1 | Out-Null
    } -ArgumentList $Server, $Port
    Start-Sleep -Seconds 2

    if ($job.State -ne "Running") {
        Write-Host "CRASH"
        return "0"
    }

    # Run benchmark
    try {
        $output = & $LoadGen -c $Conc -n $TotalRequests -p $Port -h 127.0.0.1 2>&1
        # Parse RPS from output: "RPS: <value>" or "Tput: <value> req/s"
        $rps = "0"
        if ($output -match 'RPS:\s*([\d.]+)') {
            $rps = $Matches[1]
        } elseif ($output -match 'Tput:\s*([\d.]+)') {
            $rps = $Matches[1]
        } elseif ($output -match '([\d.]+)\s*req/s') {
            $rps = $Matches[1]
        }
    } catch {
        $rps = "0"
    }

    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    return $rps
}

# ---- Main ----
$resultsFile = Join-Path $ResultDir "results.csv"
"concurrency,coronet,asio" | Out-File -FilePath $resultsFile -Encoding UTF8

foreach ($conc in $Concurrencies) {
    Write-Host "--- Concurrency: $conc ---" -ForegroundColor Yellow

    Write-Host -NoNewline "  coronet: "
    $crps = Run-OneTest -Server $CoroBin -Name "coronet" -Port $CoroPort -Conc $conc
    Write-Host "$crps req/s"

    Write-Host -NoNewline "  ASIO:    "
    $arps = Run-OneTest -Server $AsioBin -Name "ASIO" -Port $AsioPort -Conc $conc
    Write-Host "$arps req/s"

    "$conc,$crps,$arps" | Out-File -FilePath $resultsFile -Encoding UTF8 -Append
    Write-Host ""
}

# ---- Summary ----
Write-Host "============================================" -ForegroundColor Green
Write-Host "  Results: coronet vs ASIO (Windows)" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
Write-Host ("{0,-10} {1,15} {2,15} {3,10}" -f "Conc", "coronet", "ASIO", "Ratio")
Write-Host "----------------------------------------------------------"

Get-Content $resultsFile | Select-Object -Skip 1 | ForEach-Object {
    $parts = $_ -split ','
    $conc = $parts[0]
    $crps = [double]$parts[1]
    $arps = [double]$parts[2]
    if ($arps -gt 0) {
        $ratio = "{0:F2}" -f ($crps / $arps)
    } else {
        $ratio = "N/A"
    }
    Write-Host ("{0,-10} {1,15:F0} {2,15:F0} {3,10}" -f $conc, $crps, $arps, $ratio)
}

Write-Host ""
Write-Host "Results: $resultsFile" -ForegroundColor Green

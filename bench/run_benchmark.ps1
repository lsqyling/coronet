# ============================================================
# Windows PowerShell benchmark script
# Uses redis_loadgen.exe (custom load generator)
# Usage: .\run_benchmark.ps1
# ============================================================

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "..\build-release"

$CoroServer = Join-Path $BuildDir "bench\redis_echo_coro.exe"
$AsioServer = Join-Path $BuildDir "bench\redis_echo_asio.exe"
$LoadGen = Join-Path $BuildDir "bench\redis_loadgen.exe"

$CoroPort = 6379
$AsioPort = 6380

$Concurrencies = @(10, 50, 100, 200, 500, 1000, 2000, 5000)
$TotalRequests = 100000

$DataDir = Join-Path $ScriptDir "..\data"
$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$ResultDir = Join-Path $DataDir "benchmark_results_$Timestamp"
New-Item -ItemType Directory -Force -Path $ResultDir | Out-Null

Write-Host "============================================"
Write-Host "  coronet vs ASIO Redis Benchmark (Windows)"
Write-Host "============================================"
Write-Host "Results: $ResultDir"
Write-Host ""

function Run-Bench {
    param($Server, $ServerName, $Port, $CsvFile)

    Write-Host "=== Testing $ServerName on port $Port ==="

    # Start server
    $proc = Start-Process -FilePath $Server -ArgumentList $Port `
        -PassThru -NoNewWindow -WindowStyle Hidden

    Start-Sleep -Seconds 2

    if ($proc.HasExited) {
        Write-Host "ERROR: $ServerName failed to start"
        return
    }

    "concurrency,rps" | Out-File -FilePath $CsvFile -Encoding ASCII

    foreach ($conc in $Concurrencies) {
        Write-Host -NoNewline "  concurrency=$conc : "

        try {
            $output = & $LoadGen -c $conc -n $TotalRequests -p $Port 2>&1
            $rpsLine = $output | Select-String "RPS:" | Select-Object -Last 1
            if ($rpsLine) {
                $rps = ($rpsLine -split '\s+')[-1] -as [double]
            } else {
                $rps = 0
            }
            "$conc,$rps" | Out-File -FilePath $CsvFile -Append -Encoding ASCII
            Write-Host "$rps req/s"
        } catch {
            "$conc,0" | Out-File -FilePath $CsvFile -Append -Encoding ASCII
            Write-Host "FAILED"
        }

        Start-Sleep -Milliseconds 500
    }

    Stop-Process $proc -Force
    Write-Host ""
}

# Run benchmarks
Run-Bench $CoroServer "coronet (coroutine)" $CoroPort (Join-Path $ResultDir "coronet.csv")
Run-Bench $AsioServer "ASIO (callback)" $AsioPort (Join-Path $ResultDir "asio.csv")

Write-Host "============================================"
Write-Host "  Benchmark complete! Results: $ResultDir"
Write-Host "============================================"

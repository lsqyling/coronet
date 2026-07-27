# ============================================================
# Coronet vs ASIO C1000K Benchmark Script (Windows PowerShell)
# ============================================================
# Copy to build output dir during CMake build. Run directly:
#   cd buildmsvc-release\stress-test
#   powershell -ExecutionPolicy Bypass -File bench_c1000k.ps1
# ============================================================

param(
    [int]$Requests     = 1000000,   # 1M requests
    [int]$Clients      = 1000,      # 1000 concurrent
    [string]$OutputDir = ".",
    [switch]$KeepTempFiles = $false # keep stderr logs for debugging
)

$ErrorActionPreference = "Continue"
$StartTime  = Get-Date

# ============================================================
# Script is copied to build/script/win/ — find binaries in build/stress-test/
# ============================================================
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Find build root: search upward for a directory containing "stress-test/"
$BuildDir = $ScriptDir
while ($BuildDir -and -not (Test-Path (Join-Path $BuildDir "stress-test"))) {
    $parent = Split-Path -Parent $BuildDir
    if ($parent -eq $BuildDir) { break }
    $BuildDir = $parent
}
# Binary directory is build/stress-test/
$BinDir = Join-Path $BuildDir "stress-test"

# Find repo root by searching upward for "redistools"
$RepoRoot = $BuildDir
while ($RepoRoot -and -not (Test-Path (Join-Path $RepoRoot "redistools"))) {
    $parent = Split-Path -Parent $RepoRoot
    if ($parent -eq $RepoRoot) { break }
    $RepoRoot = $parent
}

# ============================================================
# Paths
# ============================================================
$BinaryNames = @{
    "coronet_ST"       = "redis_echo_ST.exe"
    "coronet_chain"    = "redis_echo_chain.exe"
    "coronet_MT"       = "redis_echo_MT.exe"
    "ASIO_ST"          = "redis_echo_asio_ST.exe"
    "ASIO_MT"          = "redis_echo_asio_MT.exe"
}

$RedisDir    = Join-Path $RepoRoot "redistools"
$RedisBench  = Join-Path $RedisDir "redis-benchmark.exe"
$RedisCli    = Join-Path $RedisDir "redis-cli.exe"

if (-not (Test-Path $RedisBench)) {
    Write-Host "ERROR: redis-benchmark.exe not found at $RedisBench" -ForegroundColor Red
    Write-Host "  Searched upward from: $BuildDir" -ForegroundColor Red
    exit 1
}

Write-Host "Build dir:  $BuildDir"
Write-Host "Redis dir:  $RedisDir"
Write-Host ""

# ============================================================
# Global state — track everything for cleanup
# ============================================================
$PortBase        = 16500
$AllServerProcs  = [System.Collections.ArrayList]::new()
$TempFiles       = [System.Collections.ArrayList]::new()
$UsedPorts       = [System.Collections.ArrayList]::new()
$Script:CleanupCompleted = $false

# Timestamp
$Timestamp   = Get-Date -Format "yyyyMMdd_HHmmss"
$ReportFile  = Join-Path $OutputDir "bench_report_${Timestamp}.txt"
$CsvFile     = Join-Path $OutputDir "bench_report_${Timestamp}.csv"

# ============================================================
# Resource Cleanup — guaranteed to run
# ============================================================
function Invoke-FullCleanup {
    if ($Script:CleanupCompleted) { return }
    $Script:CleanupCompleted = $true

    Write-Host "`n============================================================" -ForegroundColor Gray
    Write-Host "  Cleaning up resources..." -ForegroundColor Yellow
    Write-Host "============================================================" -ForegroundColor Gray

    $killed = 0
    $cleaned = 0

    # 1. Kill all tracked server processes
    foreach ($proc in $AllServerProcs) {
        try {
            if (-not $proc.HasExited) {
                $proc.Kill()
                $proc.WaitForExit(3000)
                $killed++
                Write-Host "  [KILL] $($proc.ProcessName) (PID=$($proc.Id))" -ForegroundColor Gray
            }
        } catch {}
    }

    # 2. Kill any orphaned server processes by name (belt-and-suspenders)
    $orphanNames = @(
        "redis_echo_ST", "redis_echo_chain", "redis_echo_MT",
        "redis_echo_asio_ST", "redis_echo_asio_MT",
        "redis-benchmark", "redis_loadgen", "stress_driver"
    )
    foreach ($name in $orphanNames) {
        try {
            $orphans = Get-Process -Name $name -ErrorAction SilentlyContinue
            foreach ($p in $orphans) {
                $p.Kill()
                $killed++
                Write-Host "  [ORPHAN] $name (PID=$($p.Id))" -ForegroundColor Gray
            }
        } catch {}
    }

    # 3. Wait for sockets to release (TIME_WAIT -> CLOSED)
    Start-Sleep -Seconds 2

    # 4. Clean temp files (unless --KeepTempFiles is set)
    if (-not $KeepTempFiles) {
        foreach ($file in $TempFiles) {
            if (Test-Path $file) {
                Remove-Item $file -Force -ErrorAction SilentlyContinue
                $cleaned++
            }
        }
        # Clean temp dir by pattern
        $pattern = "$env:TEMP\coronet_bench_*"
        Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue | ForEach-Object {
            Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
            $cleaned++
        }
    } else {
        Write-Host "  [KEEP] Temp files preserved (--KeepTempFiles)" -ForegroundColor Gray
        foreach ($file in $TempFiles) {
            if (Test-Path $file) {
                Write-Host "    $file" -ForegroundColor Gray
            }
        }
    }

    # 5. Summary
    Write-Host "  Cleanup done: $killed processes killed, $cleaned temp files removed" -ForegroundColor Green
}

# ============================================================
# Helper: Check if a port is free
# ============================================================
function Test-PortFree {
    param([int]$Port)
    try {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
        $listener.Start()
        $listener.Stop()
        return $true
    } catch {
        return $false
    }
}

# ============================================================
# Helper: Test one server
# ============================================================
function Test-Server {
    param(
        [string]$Name,
        [string]$Binary,
        [int]$Port,
        [string]$ExtraArgs = ""
    )

    $serverProc = $null
    $stderrFile = $null

    try {
        if (-not (Test-Path $Binary)) {
            $msg = "  $Name [port $Port] SKIP (binary not found)"
            Write-Host $msg
            Add-Content -Path $ReportFile -Value $msg
            return @{ ok = $false; rps = 0; cpu = 0; mem = 0 }
        }

        # Check port is free before starting
        if (-not (Test-PortFree $Port)) {
            Write-Host "  $Name [port $Port] SKIP (port in use)" -ForegroundColor Yellow
            Add-Content -Path $ReportFile -Value "  $Name [port $Port] SKIP (port $Port already in use)"
            return @{ ok = $false; rps = 0; cpu = 0; mem = 0 }
        }
        $null = $UsedPorts.Add($Port)

        Write-Host -NoNewline "  $Name [port $Port] "
        Add-Content -Path $ReportFile -Value "  $Name [port $Port] starting..."

        # Spawn server
        $stderrFile = "$env:TEMP\coronet_bench_${Name}_$(Get-Date -Format 'HHmmss').txt"
        $null = $TempFiles.Add($stderrFile)

        $serverProc = Start-Process -FilePath $Binary `
            -ArgumentList "$Port $ExtraArgs" `
            -NoNewWindow -PassThru `
            -RedirectStandardError $stderrFile
        $null = $AllServerProcs.Add($serverProc)

        Start-Sleep -Milliseconds 500

        # Check for immediate crash
        if ($serverProc.HasExited) {
            $stderr = if (Test-Path $stderrFile) { Get-Content $stderrFile -Raw } else { "" }
            Write-Host "CRASH (exit=$($serverProc.ExitCode))" -ForegroundColor Red
            if ($stderr) {
                Write-Host "        stderr: $($stderr.Trim())" -ForegroundColor DarkYellow
            }
            Add-Content -Path $ReportFile -Value "CRASH (exit=$($serverProc.ExitCode))`nstderr: $stderr"
            return @{ ok = $false; rps = 0; cpu = 0; mem = 0 }
        }

        # Wait for port ready (up to 10 seconds)
        $ready = $false
        for ($i = 0; $i -lt 50; $i++) {
            Start-Sleep -Milliseconds 200
            $pingResult = & $RedisCli -p $Port PING 2>$null
            if ($pingResult -match "PONG") {
                $ready = $true
                break
            }
            if ($serverProc.HasExited) {
                $stderr = if (Test-Path $stderrFile) { Get-Content $stderrFile -Raw } else { "" }
                Write-Host "CRASH (exit=$($serverProc.ExitCode))" -ForegroundColor Red
                if ($stderr) {
                    Write-Host "        stderr: $($stderr.Trim())" -ForegroundColor DarkYellow
                }
                Add-Content -Path $ReportFile -Value "CRASH during wait (exit=$($serverProc.ExitCode))`nstderr: $stderr"
                return @{ ok = $false; rps = 0; cpu = 0; mem = 0 }
            }
        }

        if (-not $ready) {
            Write-Host "TIMEOUT (port not ready after 10s)" -ForegroundColor Yellow
            Add-Content -Path $ReportFile -Value "TIMEOUT (port $Port not ready after 10s)"
            return @{ ok = $false; rps = 0; cpu = 0; mem = 0 }
        }

        # ---- Run benchmark ----
        $benchSw = [System.Diagnostics.Stopwatch]::StartNew()
        $benchOutput = & $RedisBench -h 127.0.0.1 -p $Port -n $Requests -c $Clients -t ping -q 2>&1
        $benchSw.Stop()
        $outputStr = $benchOutput -join "`n"
        $elapsed = [math]::Round($benchSw.Elapsed.TotalSeconds, 1)

        # Parse RPS
        $rps = 0
        if ($outputStr -match "([\d.]+)\s*requests per second") {
            $rps = [double]$Matches[1]
        }

        # Sample CPU/memory
        $cpu = 0; $mem = 0
        try {
            $p = Get-Process -Id $serverProc.Id -ErrorAction SilentlyContinue
            if ($p) {
                $cpu = [math]::Round($p.CPU, 1)
                $mem = [math]::Round($p.WorkingSet64 / 1MB, 0)
            }
        } catch {}

        # Print result
        if ($rps -gt 0) {
            $resultLine = "PASS  rps=$([math]::Round($rps,0))  cpu=${cpu}%  mem=${mem}MB  time=${elapsed}s"
            Write-Host $resultLine -ForegroundColor Green
        } else {
            $resultLine = "FAIL (elapsed=${elapsed}s)"
            Write-Host $resultLine -ForegroundColor Red
        }

        Add-Content -Path $ReportFile -Value $resultLine
        Add-Content -Path $CsvFile -Value "$Name,$Port,$([math]::Round($rps,0)),$cpu,$mem,$(if($rps -gt 0){'PASS'}else{'FAIL'}),$elapsed"

        return @{ ok = ($rps -gt 0); rps = $rps; cpu = $cpu; mem = $mem }

    } finally {
        # Per-server cleanup — always kill the server process
        if ($serverProc -and -not $serverProc.HasExited) {
            try {
                $serverProc.Kill()
                $serverProc.WaitForExit(2000)
            } catch {}
        }
    }
}

# ============================================================
# Main — wrapped in try/finally for guaranteed cleanup
# ============================================================

$ranMain = $false

try {
    # ---- Init report ----
    "Name,Port,RPS,CPU%,MemMB,Status,ElapsedSec" | Out-File -FilePath $CsvFile -Encoding utf8

    $header = @"

============================================================
  Coronet vs ASIO C1000K Benchmark
============================================================
  Date:     $(Get-Date)
  Requests: $Requests
  Clients:  $Clients
  Pipeline: 1 (no pipelining)
  Build:    $BuildDir
  Report:   $ReportFile
============================================================

"@
    Write-Host $header
    Add-Content -Path $ReportFile -Value $header

    # ---- Round 1: Single-Threaded ----
    Write-Host "=== Round 1: Single-Threaded ===" -ForegroundColor Cyan
    Add-Content -Path $ReportFile -Value "`n=== Round 1: Single-Threaded ==="

    $r1a = Test-Server "coronet_ST"       (Join-Path $BinDir $BinaryNames["coronet_ST"])    ($PortBase)
    $r1b = Test-Server "coronet_chain"    (Join-Path $BinDir $BinaryNames["coronet_chain"]) ($PortBase + 1)
    $r1c = Test-Server "ASIO_ST"          (Join-Path $BinDir $BinaryNames["ASIO_ST"])       ($PortBase + 2)

    # ---- Round 2: Multi-Threaded ----
    Write-Host "`n=== Round 2: Multi-Threaded (6 threads) ===" -ForegroundColor Cyan
    Add-Content -Path $ReportFile -Value "`n=== Round 2: Multi-Threaded (6 threads) ==="

    $r2a = Test-Server "coronet_MT(6)"    (Join-Path $BinDir $BinaryNames["coronet_MT"])    ($PortBase + 10) "6"
    $r2b = Test-Server "ASIO_MT(6)"       (Join-Path $BinDir $BinaryNames["ASIO_MT"])       ($PortBase + 11) "6"

    # ---- Compute stats ----
    $totalTime = [math]::Round(((Get-Date) - $StartTime).TotalMinutes, 1)
    $allResults = @($r1a, $r1b, $r1c, $r2a, $r2b)
    $passed  = ($allResults | Where-Object { $_.ok }).Count
    $failed  = ($allResults | Where-Object { -not $_.ok }).Count
    # Manual sum (Measure-Object -Property doesn't work on hashtable keys)
    $rpsSum = 0
    ($allResults | Where-Object { $_.ok }) | ForEach-Object { $rpsSum += $_.rps }
    $avgRps = if ($passed -gt 0) { [math]::Round($rpsSum / $passed, 0) } else { 0 }

    # Comparison calculations
    $stVsAsio = if ($r1a.ok -and $r1c.ok -and $r1c.rps -gt 0) {
        [math]::Round(($r1a.rps - $r1c.rps) / $r1c.rps * 100, 1)
    } else { "N/A" }
    $mtVsAsio = if ($r2a.ok -and $r2b.ok -and $r2b.rps -gt 0) {
        [math]::Round(($r2a.rps - $r2b.rps) / $r2b.rps * 100, 1)
    } else { "N/A" }

    # ---- Summary ----
    $summary = @"

============================================================
  BENCHMARK SUMMARY
============================================================
  Duration:  ${totalTime} minutes
  Passed:    ${passed} / $($allResults.Count)
  Avg RPS:   ${avgRps}

  Single-Threaded:
    coronet_ST:     RPS=$([math]::Round($r1a.rps,0))  CPU=$($r1a.cpu)%  MEM=$($r1a.mem)MB
    coronet_chain:  RPS=$([math]::Round($r1b.rps,0))  CPU=$($r1b.cpu)%  MEM=$($r1b.mem)MB
    ASIO_ST:        RPS=$([math]::Round($r1c.rps,0))  CPU=$($r1c.cpu)%  MEM=$($r1c.mem)MB
    coronet_ST vs ASIO: ${stVsAsio}%

  Multi-Threaded (6 threads):
    coronet_MT(6):  RPS=$([math]::Round($r2a.rps,0))  CPU=$($r2a.cpu)%  MEM=$($r2a.mem)MB
    ASIO_MT(6):     RPS=$([math]::Round($r2b.rps,0))  CPU=$($r2b.cpu)%  MEM=$($r2b.mem)MB
    coronet_MT vs ASIO: ${mtVsAsio}%

  Ports used: $($UsedPorts -join ', ')

  Report: $ReportFile
  CSV:    $CsvFile
============================================================

"@
    Write-Host $summary
    Add-Content -Path $ReportFile -Value $summary

    $ranMain = $true

} catch {
    Write-Host "`n[FATAL] Benchmark script failed: $_" -ForegroundColor Red
    Add-Content -Path $ReportFile -Value "`n[FATAL] Script error: $_"
    $ranMain = $false

} finally {
    # ============================================================
    # GUARANTEED CLEANUP — runs even on Ctrl+C or script error
    # ============================================================
    Invoke-FullCleanup

    if ($ranMain) {
        Write-Host "`nBenchmark complete. Resources cleaned up." -ForegroundColor Green
    } else {
        Write-Host "`nBenchmark aborted. Resources cleaned up." -ForegroundColor Yellow
    }
    Write-Host "Report: $ReportFile"
    Write-Host "CSV:    $CsvFile"
}

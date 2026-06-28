# Windows performance comparison: coronet vs ASIO
param($OutputCsv = "D:\dev\workspace\yidaoyun\coronet\data\perf_win.csv")

$BIN = "D:\dev\workspace\yidaoyun\coronet\build\bench\Release"
$CONCS = @(10, 50, 100, 200, 500)
$TOTAL = 100000

"server,type,concurrency,rps,stable" | Out-File $OutputCsv

function Test-ST($name, $exe, $port) {
    foreach ($c in $CONCS) {
        taskkill /f /im (Split-Path $exe -Leaf) 2>$null
        Start-Sleep 1
        $p = Start-Process $exe -Arg $port -PassThru -WindowStyle Hidden
        Start-Sleep 3
        if ($p.HasExited) {
            Write-Host "  [$name ST c=$c] CRASH"
            "$name,ST,$c,0,0" | Out-File $OutputCsv -Append
            continue
        }
        $r = & "$BIN\redis_loadgen.exe" -c $c -n $TOTAL -p $port 2>&1
        $rps = if ($r -match "RPS:\s+([\d.]+)") { $Matches[1] } else { "0" }
        $stable = if ($p.HasExited) { 0 } else { 1 }
        Write-Host "  [$name ST c=$c] RPS=$rps stable=$stable"
        "$name,ST,$c,$rps,$stable" | Out-File $OutputCsv -Append
        $p.Kill(); Start-Sleep 1
    }
}

function Test-MT($name, $exe, $port) {
    foreach ($c in $CONCS) {
        taskkill /f /im (Split-Path $exe -Leaf) 2>$null
        Start-Sleep 2
        if ($name -like "*asio*") {
            $p = Start-Process $exe -Arg "$port", "6" -PassThru -WindowStyle Hidden
        } else {
            $p = Start-Process $exe -PassThru -WindowStyle Hidden  # coronet MT hardcodes 6379
        }
        Start-Sleep 4
        if ($p.HasExited) {
            Write-Host "  [$name MT c=$c] CRASH"
            "$name,MT,$c,0,0" | Out-File $OutputCsv -Append
            continue
        }
        $r = & "$BIN\redis_loadgen.exe" -c $c -n $TOTAL -p $port 2>&1
        $rps = if ($r -match "RPS:\s+([\d.]+)") { $Matches[1] } else { "0" }
        $stable = if ($p.HasExited) { 0 } else { 1 }
        Write-Host "  [$name MT c=$c] RPS=$rps stable=$stable"
        "$name,MT,$c,$rps,$stable" | Out-File $OutputCsv -Append
        $p.Kill(); Start-Sleep 2
    }
}

Write-Host "=== Windows ST ==="
Test-ST "coronet_msvc"   "$BIN\redis_echo_coro.exe"     9500
Test-ST "asio_msvc"      "$BIN\redis_echo_asio.exe"      9501

Write-Host "=== Windows MT ==="
Test-MT "coronet_msvc_mt" "$BIN\redis_echo_MT.exe"        6379
Test-MT "asio_msvc_mt"    "$BIN\redis_echo_asio_MT.exe"   6379

Write-Host "=== DONE ==="
Get-Content $OutputCsv

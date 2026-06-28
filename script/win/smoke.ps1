# Smoke test all examples on Windows
$ErrorActionPreference = "Continue"
$BIN = "build\examples\Release"
$PASS = 0; $FAIL = 0

function Kill-All { Get-Process example_* -ErrorAction Ignore | Stop-Process -Force }

function Test-Quick($name, $timeout, [scriptblock]$check) {
    Write-Host -NoNewline "  $name ... "
    $p = Start-Process -FilePath "$BIN\example_$name.exe" -PassThru -WindowStyle Hidden
    Start-Sleep 3
    if ($p.HasExited) { Write-Host "FAIL (exited)"; $global:FAIL++ }
    else {
        $ok = & $check
        if ($ok) { Write-Host "OK"; $global:PASS++ }
        else { Write-Host "FAIL"; $global:FAIL++ }
    }
    $p.Kill(); Start-Sleep 1
}

function Test-Net($name, $port, $args) {
    Write-Host -NoNewline "  $name (port $port) ... "
    Kill-All; Start-Sleep 1
    if ($args) { $p = Start-Process "$BIN\example_$name.exe" -Arg $args -PassThru -WindowStyle Hidden }
    else { $p = Start-Process "$BIN\example_$name.exe" -Arg $port -PassThru -WindowStyle Hidden }
    Start-Sleep 3
    if ($p.HasExited) { Write-Host "FAIL (crashed)"; $global:FAIL++; return }
    $r = & "$BIN\..\..\bench\Release\redis_loadgen.exe" -c 1 -n 5 -p $port 2>&1
    if ($r -match "RPS:") { Write-Host "OK ($($matches[0]))"; $global:PASS++ }
    else { Write-Host "FAIL"; $global:FAIL++ }
    $p.Kill(); Start-Sleep 1
}

function Test-Curl($name, $port, $args) {
    Write-Host -NoNewline "  $name (port $port) ... "
    Kill-All; Start-Sleep 1
    if ($args) { $p = Start-Process "$BIN\example_$name.exe" -Arg $args -PassThru -WindowStyle Hidden }
    else { $p = Start-Process "$BIN\example_$name.exe" -Arg $port -PassThru -WindowStyle Hidden }
    Start-Sleep 3
    if ($p.HasExited) { Write-Host "FAIL (crashed)"; $global:FAIL++; return }
    $r = curl -s "http://127.0.0.1:$port/" 2>&1
    if ($r -match "coronet\|test\|<html") { Write-Host "OK"; $global:PASS++ }
    else { Write-Host "FAIL ($r)"; $global:FAIL++ }
    $p.Kill(); Start-Sleep 1
}

Write-Host "=== Windows Smoke Tests ==="

# Quick exit test
Write-Host "`n--- Quick Exit ---"
$r = & "$BIN\example_iota.exe" 2>&1
if ($r -eq "6 7 8") { Write-Host "  iota: OK"; $PASS++ } else { Write-Host "  iota: FAIL"; $FAIL++ }

# Timeout-dependent (check alive after 3s)
Write-Host "`n--- Long-running ---"
Test-Quick "timer" -check { $true }
Test-Quick "mutex" -check { $true }
Test-Quick "channel" -check { $true }
Test-Quick "cv_notify_all" -check { $true }
Test-Quick "cv_notify_one" -check { $true }
Test-Quick "sem" -check { $true }
Test-Quick "timer_accuracy" -check { $true }

# when_* (complete on their own)
Write-Host "`n--- when_* ---"
$r = & { timeout 8; & "$BIN\example_when_all.exe" } 2>&1
if ($r -match "f1 Great") { Write-Host "  when_all: OK"; $PASS++ } else { Write-Host "  when_all: FAIL"; $FAIL++ }
$r = & { timeout 6; & "$BIN\example_when_any.exe" } 2>&1
if ($r -match "f1 Great") { Write-Host "  when_any: OK"; $PASS++ } else { Write-Host "  when_any: FAIL"; $FAIL++ }
$r = & { timeout 8; & "$BIN\example_when_some.exe" } 2>&1
if ($r -match "f1 Great") { Write-Host "  when_some: OK"; $PASS++ } else { Write-Host "  when_some: FAIL"; $FAIL++ }

# Network servers
Write-Host "`n--- Network ---"
Test-Net "echo_server" 9880
Test-Net "echo_server_MT" 6379
mkdir -p public; echo "<h1>coronet</h1>" > public/index.html
Test-Curl "httpd" 9882
Test-Curl "httpd_MT" 9883

# netcat
Write-Host "`n--- netcat ---"
$r = & { timeout 2; & "$BIN\example_netcat.exe" } 2>&1
if ($r -match "Usage") { Write-Host "  netcat: OK"; $PASS++ } else { Write-Host "  netcat: FAIL"; $FAIL++ }

# pingpong_client
Write-Host "`n--- pingpong ---"
Test-Quick "pingpong_client" -check {
    # needs echo_server on 9527
    $srv = Start-Process "$BIN\example_echo_server.exe" -Arg 9527 -PassThru -WindowStyle Hidden
    Start-Sleep 2
    $ok = !$srv.HasExited
    $srv.Kill()
    $ok
}

Write-Host "`n=== Results: $PASS passed, $FAIL failed ==="
Kill-All

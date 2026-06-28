# ============================================================
# Master script: run all Windows benchmarks
# Usage: powershell -File script/windows/run_all.ps1 [-Smoke] [-Full] [-Scale]
# ============================================================
param(
    [switch]$Smoke,
    [switch]$Full,
    [switch]$Scale
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Smoke) {
    Write-Host "Running smoke test..." -ForegroundColor Cyan
    & (Join-Path $ScriptDir "smoke_test.ps1")
}
elseif ($Full) {
    Write-Host "Running fair benchmark..." -ForegroundColor Cyan
    & (Join-Path $ScriptDir "fair_bench.ps1")
}
elseif ($Scale) {
    Write-Host "Running scaling test..." -ForegroundColor Cyan
    & (Join-Path $ScriptDir "scaling_test.ps1")
}
else {
    Write-Host "Usage: run_all.ps1 [-Smoke] [-Full] [-Scale]" -ForegroundColor Yellow
    Write-Host "  -Smoke : quick smoke test (200 req, c=10)" -ForegroundColor Yellow
    Write-Host "  -Full  : full fair benchmark (100K req, c=10..1000)" -ForegroundColor Yellow
    Write-Host "  -Scale : step-by-step scaling test" -ForegroundColor Yellow
}

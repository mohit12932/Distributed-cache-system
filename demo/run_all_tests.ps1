# ============================================================================
# Distributed Cache System - Complete Test Suite Runner
# ============================================================================
# This script builds and runs all tests, then starts a live demo server.
# Usage: .\demo\run_all_tests.ps1
# ============================================================================

param(
    [switch]$SkipBuild,
    [switch]$SkipServer,
    [int]$Port = 6399
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $ProjectRoot

Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║     Distributed Cache System - Complete Test Suite               ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# ── Build Phase ─────────────────────────────────────────────────────────────

if (-not $SkipBuild) {
    Write-Host "📦 Building project..." -ForegroundColor Yellow
    
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" | Out-Null
    }
    
    $buildCommands = @(
        @{Name="Main Server"; Cmd="g++ -std=c++17 -O2 -Wall -I. -o build/distributed_cache.exe src/main.cpp -lws2_32"},
        @{Name="LRU Cache Tests"; Cmd="g++ -std=c++17 -O2 -Wall -I. -o build/test_lru_cache.exe src/tests/test_lru_cache.cpp -lws2_32"},
        @{Name="Concurrency Tests"; Cmd="g++ -std=c++17 -O2 -Wall -I. -o build/test_concurrency.exe src/tests/test_concurrency.cpp -lws2_32"},
        @{Name="RESP Parser Tests"; Cmd="g++ -std=c++17 -O2 -Wall -I. -o build/test_resp_parser.exe src/tests/test_resp_parser.cpp -lws2_32"},
        @{Name="Live Server Tests"; Cmd="g++ -std=c++17 -O2 -Wall -I. -o build/test_live_server.exe tests/test_live_server.cpp -lws2_32"}
    )
    
    foreach ($build in $buildCommands) {
        Write-Host "  Building $($build.Name)..." -NoNewline
        $result = Invoke-Expression $build.Cmd 2>&1
        if ($LASTEXITCODE -eq 0 -or (Test-Path "build/$($build.Name -replace ' ','_').exe" -ErrorAction SilentlyContinue)) {
            Write-Host " ✓" -ForegroundColor Green
        } else {
            Write-Host " ✗" -ForegroundColor Red
            Write-Host $result -ForegroundColor Red
        }
    }
    Write-Host ""
}

# ── Test Suite 1: LRU Cache Core ────────────────────────────────────────────

Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
Write-Host "  TEST SUITE 1: LRU Cache Core Engine" -ForegroundColor White
Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
& .\build\test_lru_cache.exe
$suite1Pass = $LASTEXITCODE -eq 0
Write-Host ""

# ── Test Suite 2: Concurrency ───────────────────────────────────────────────

Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
Write-Host "  TEST SUITE 2: Concurrency Stress Tests" -ForegroundColor White
Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
& .\build\test_concurrency.exe
$suite2Pass = $LASTEXITCODE -eq 0
Write-Host ""

# ── Test Suite 3: RESP Protocol ─────────────────────────────────────────────

Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
Write-Host "  TEST SUITE 3: RESP Parser & Handler Tests" -ForegroundColor White
Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
& .\build\test_resp_parser.exe
$suite3Pass = $LASTEXITCODE -eq 0
Write-Host ""

# ── Test Suite 4: Live Server ───────────────────────────────────────────────

Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White
Write-Host "  TEST SUITE 4: Live Server Integration" -ForegroundColor White
Write-Host "═══════════════════════════════════════════════════════════════════" -ForegroundColor White

# Clean up old data
Remove-Item -Recurse -Force "data" -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "test_data" -ErrorAction SilentlyContinue

# Start server in background
Write-Host "  Starting server on port $Port..." -ForegroundColor Gray
$serverProcess = Start-Process -FilePath ".\build\distributed_cache.exe" `
    -ArgumentList "--port $Port --mode write-through --data-file data/test.dat" `
    -PassThru -WindowStyle Hidden

Start-Sleep -Seconds 2

# Run integration tests
& .\build\test_live_server.exe
$suite4Pass = $LASTEXITCODE -eq 0

# Stop server
Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
Write-Host ""

# ── Summary ─────────────────────────────────────────────────────────────────

Write-Host "╔══════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║                        TEST SUMMARY                              ║" -ForegroundColor Cyan
Write-Host "╠══════════════════════════════════════════════════════════════════╣" -ForegroundColor Cyan

$suites = @(
    @{Name="LRU Cache Core"; Pass=$suite1Pass; Tests=11},
    @{Name="Concurrency Stress"; Pass=$suite2Pass; Tests=5},
    @{Name="RESP Parser & Handler"; Pass=$suite3Pass; Tests=16},
    @{Name="Live Server Integration"; Pass=$suite4Pass; Tests=26}
)

$totalTests = 0
$totalPass = 0

foreach ($suite in $suites) {
    $status = if ($suite.Pass) { "✓ PASS" } else { "✗ FAIL" }
    $color = if ($suite.Pass) { "Green" } else { "Red" }
    $line = "║  {0,-25} {1,3} tests  {2,-8}               ║" -f $suite.Name, $suite.Tests, $status
    Write-Host $line -ForegroundColor $color
    $totalTests += $suite.Tests
    if ($suite.Pass) { $totalPass += $suite.Tests }
}

Write-Host "╠══════════════════════════════════════════════════════════════════╣" -ForegroundColor Cyan
$totalStatus = if ($totalPass -eq $totalTests) { "ALL PASSED" } else { "SOME FAILED" }
$totalColor = if ($totalPass -eq $totalTests) { "Green" } else { "Red" }
Write-Host ("║  TOTAL: {0}/{1} tests passed - {2,-20}           ║" -f $totalPass, $totalTests, $totalStatus) -ForegroundColor $totalColor
Write-Host "╚══════════════════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# ── Optional: Start Demo Server ─────────────────────────────────────────────

if (-not $SkipServer) {
    Write-Host "🚀 Starting demo server on port 6379..." -ForegroundColor Yellow
    Write-Host "   Connect with: redis-cli -p 6379" -ForegroundColor Gray
    Write-Host "   Press Ctrl+C to stop" -ForegroundColor Gray
    Write-Host ""
    & .\build\distributed_cache.exe --port 6379 --mode write-back
}

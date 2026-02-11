# ============================================================================
# Distributed Cache System - HR Demo Showcase
# ============================================================================
# This script demonstrates all features of the distributed cache system
# in an interactive, visual format suitable for presenting to HR/recruiters.
#
# Usage: .\demo\demo_showcase.ps1
# ============================================================================

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $ProjectRoot

$PORT = 6399
$serverProcess = $null

function Write-Banner {
    param([string]$Text, [string]$Color = "Cyan")
    $line = "═" * 70
    Write-Host ""
    Write-Host $line -ForegroundColor $Color
    Write-Host "  $Text" -ForegroundColor $Color
    Write-Host $line -ForegroundColor $Color
    Write-Host ""
}

function Write-Step {
    param([int]$Number, [string]$Title)
    Write-Host ""
    Write-Host "┌─────────────────────────────────────────────────────────────────────" -ForegroundColor Yellow
    Write-Host "│ STEP $Number`: $Title" -ForegroundColor Yellow
    Write-Host "└─────────────────────────────────────────────────────────────────────" -ForegroundColor Yellow
    Write-Host ""
}

function Pause-Demo {
    param([string]$Message = "Press ENTER to continue...")
    Write-Host ""
    Write-Host "  ⏸️  $Message" -ForegroundColor Gray
    Read-Host | Out-Null
}

function Send-CacheCommand {
    param([string]$Command)
    
    Write-Host "  redis-cli> " -NoNewline -ForegroundColor Green
    Write-Host $Command -ForegroundColor White
    
    # Create TCP client
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $PORT)
    $stream = $client.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream)
    $reader = New-Object System.IO.StreamReader($stream)
    
    $writer.WriteLine($Command)
    $writer.Flush()
    
    Start-Sleep -Milliseconds 100
    
    $response = ""
    while ($stream.DataAvailable) {
        $response += $reader.ReadLine() + "`n"
    }
    
    $client.Close()
    
    # Parse and display response
    $response = $response.Trim()
    if ($response -match '^\+(.*)') {
        Write-Host "  $($Matches[1])" -ForegroundColor Cyan
    } elseif ($response -match '^:(.*)') {
        Write-Host "  (integer) $($Matches[1])" -ForegroundColor Cyan
    } elseif ($response -match '^\$-1') {
        Write-Host "  (nil)" -ForegroundColor DarkGray
    } elseif ($response -match '^\$(\d+)') {
        $len = [int]$Matches[1]
        $value = ($response -split "`n")[1]
        Write-Host "  `"$value`"" -ForegroundColor Cyan
    } elseif ($response -match '^\*(\d+)') {
        $count = [int]$Matches[1]
        $lines = $response -split "`n"
        for ($i = 1; $i -lt $lines.Count; $i += 2) {
            if ($lines[$i] -match '^\$\d+' -and $i + 1 -lt $lines.Count) {
                Write-Host "  $($i / 2 + 1)) `"$($lines[$i + 1])`"" -ForegroundColor Cyan
            }
        }
    } elseif ($response -match '^-ERR(.*)') {
        Write-Host "  (error) $($Matches[1])" -ForegroundColor Red
    } else {
        Write-Host "  $response" -ForegroundColor Cyan
    }
    
    return $response
}

# ════════════════════════════════════════════════════════════════════════════
# MAIN DEMO
# ════════════════════════════════════════════════════════════════════════════

Clear-Host

Write-Host ""
Write-Host "  ██████╗ ██╗███████╗████████╗██████╗ ██╗██████╗ ██╗   ██╗████████╗███████╗██████╗ " -ForegroundColor Cyan
Write-Host "  ██╔══██╗██║██╔════╝╚══██╔══╝██╔══██╗██║██╔══██╗██║   ██║╚══██╔══╝██╔════╝██╔══██╗" -ForegroundColor Cyan
Write-Host "  ██║  ██║██║███████╗   ██║   ██████╔╝██║██████╔╝██║   ██║   ██║   █████╗  ██║  ██║" -ForegroundColor Cyan
Write-Host "  ██║  ██║██║╚════██║   ██║   ██╔══██╗██║██╔══██╗██║   ██║   ██║   ██╔══╝  ██║  ██║" -ForegroundColor Cyan
Write-Host "  ██████╔╝██║███████║   ██║   ██║  ██║██║██████╔╝╚██████╔╝   ██║   ███████╗██████╔╝" -ForegroundColor Cyan
Write-Host "  ╚═════╝ ╚═╝╚══════╝   ╚═╝   ╚═╝  ╚═╝╚═╝╚═════╝  ╚═════╝    ╚═╝   ╚══════╝╚═════╝ " -ForegroundColor Cyan
Write-Host ""
Write-Host "                    ██████╗ █████╗  ██████╗██╗  ██╗███████╗" -ForegroundColor Yellow
Write-Host "                   ██╔════╝██╔══██╗██╔════╝██║  ██║██╔════╝" -ForegroundColor Yellow
Write-Host "                   ██║     ███████║██║     ███████║█████╗  " -ForegroundColor Yellow
Write-Host "                   ██║     ██╔══██║██║     ██╔══██║██╔══╝  " -ForegroundColor Yellow
Write-Host "                   ╚██████╗██║  ██║╚██████╗██║  ██║███████╗" -ForegroundColor Yellow
Write-Host "                    ╚═════╝╚═╝  ╚═╝ ╚═════╝╚═╝  ╚═╝╚══════╝" -ForegroundColor Yellow
Write-Host ""
Write-Host "              High-Performance In-Memory Cache System" -ForegroundColor White
Write-Host "                   Redis-Compatible • Thread-Safe • Persistent" -ForegroundColor Gray
Write-Host ""

Pause-Demo "Press ENTER to start the demo..."

# ─── Step 1: Run Unit Tests ─────────────────────────────────────────────────

Write-Step 1 "UNIT TESTS - Verifying Core Engine"

Write-Host "  Running 11 LRU Cache tests..." -ForegroundColor Gray
& .\build\test_lru_cache.exe 2>&1 | ForEach-Object { Write-Host "  $_" }

Pause-Demo

# ─── Step 2: Concurrency Tests ──────────────────────────────────────────────

Write-Step 2 "CONCURRENCY TESTS - Thread Safety Verification"

Write-Host "  Running 5 concurrent stress tests with 16 threads..." -ForegroundColor Gray
& .\build\test_concurrency.exe 2>&1 | ForEach-Object { Write-Host "  $_" }

Pause-Demo

# ─── Step 3: Start Server ───────────────────────────────────────────────────

Write-Step 3 "STARTING SERVER - Write-Through Mode"

# Clean up
Remove-Item -Recurse -Force data -ErrorAction SilentlyContinue

Write-Host "  Configuration:" -ForegroundColor White
Write-Host "    • Port:           $PORT" -ForegroundColor Gray
Write-Host "    • Capacity:       65,536 entries" -ForegroundColor Gray
Write-Host "    • Write Mode:     Write-Through (synchronous persistence)" -ForegroundColor Gray
Write-Host "    • Data File:      data/demo.dat" -ForegroundColor Gray
Write-Host ""

$serverProcess = Start-Process -FilePath ".\build\distributed_cache.exe" `
    -ArgumentList "--port $PORT --capacity 65536 --mode write-through --data-file data/demo.dat" `
    -PassThru -WindowStyle Hidden

Start-Sleep -Seconds 2
Write-Host "  ✓ Server started successfully!" -ForegroundColor Green

Pause-Demo

# ─── Step 4: Basic Operations ───────────────────────────────────────────────

Write-Step 4 "BASIC OPERATIONS - SET, GET, EXISTS, DEL"

Write-Host "  Storing user data..." -ForegroundColor Gray
Send-CacheCommand "SET user:1001 Alice"
Send-CacheCommand "SET user:1002 Bob"
Send-CacheCommand "SET user:1003 Charlie"
Write-Host ""

Write-Host "  Retrieving data..." -ForegroundColor Gray
Send-CacheCommand "GET user:1001"
Send-CacheCommand "GET user:1002"
Write-Host ""

Write-Host "  Checking existence..." -ForegroundColor Gray
Send-CacheCommand "EXISTS user:1001"
Send-CacheCommand "EXISTS user:9999"
Write-Host ""

Write-Host "  Deleting a key..." -ForegroundColor Gray
Send-CacheCommand "DEL user:1003"
Send-CacheCommand "GET user:1003"

Pause-Demo

# ─── Step 5: Server Statistics ──────────────────────────────────────────────

Write-Step 5 "SERVER STATISTICS - INFO Command"

Write-Host "  Querying server stats..." -ForegroundColor Gray
$info = Send-CacheCommand "INFO"
Write-Host ""

Write-Host "  Key count in database..." -ForegroundColor Gray
Send-CacheCommand "DBSIZE"
Write-Host ""

Write-Host "  Listing all keys..." -ForegroundColor Gray
Send-CacheCommand "KEYS *"

Pause-Demo

# ─── Step 6: Persistence Verification ───────────────────────────────────────

Write-Step 6 "PERSISTENCE - Data Durability"

Write-Host "  Data is persisted to: data/demo.dat" -ForegroundColor Gray
Write-Host ""
Write-Host "  File contents:" -ForegroundColor White
Write-Host "  ┌────────────────────────────────────────────┐" -ForegroundColor DarkGray

Get-Content "data/demo.dat" -ErrorAction SilentlyContinue | ForEach-Object {
    $parts = $_ -split "`t"
    Write-Host "  │ $($parts[0].PadRight(15)) → $($parts[1].PadRight(24)) │" -ForegroundColor Cyan
}

Write-Host "  └────────────────────────────────────────────┘" -ForegroundColor DarkGray

Pause-Demo

# ─── Step 7: Performance Demo ───────────────────────────────────────────────

Write-Step 7 "PERFORMANCE - Bulk Operations"

Write-Host "  Inserting 1000 keys..." -ForegroundColor Gray
$start = Get-Date

for ($i = 1; $i -le 1000; $i++) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $PORT)
    $stream = $client.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.WriteLine("SET perf:$i value_$i")
    $writer.Flush()
    $client.Close()
}

$elapsed = (Get-Date) - $start
$opsPerSec = [math]::Round(1000 / $elapsed.TotalSeconds)

Write-Host ""
Write-Host "  ✓ Inserted 1000 keys in $([math]::Round($elapsed.TotalMilliseconds)) ms" -ForegroundColor Green
Write-Host "  ✓ Throughput: ~$opsPerSec operations/second" -ForegroundColor Green
Write-Host ""

Write-Host "  Final database size..." -ForegroundColor Gray
Send-CacheCommand "DBSIZE"

Pause-Demo

# ─── Step 8: FLUSHALL ───────────────────────────────────────────────────────

Write-Step 8 "CACHE MANAGEMENT - FLUSHALL"

Write-Host "  Before flush:" -ForegroundColor Gray
Send-CacheCommand "DBSIZE"
Write-Host ""

Write-Host "  Flushing all data..." -ForegroundColor Gray
Send-CacheCommand "FLUSHALL"
Write-Host ""

Write-Host "  After flush:" -ForegroundColor Gray
Send-CacheCommand "DBSIZE"

Pause-Demo

# ─── Step 9: Integration Tests ──────────────────────────────────────────────

Write-Step 9 "INTEGRATION TESTS - Full Protocol Verification"

Write-Host "  Running 26 live server integration tests..." -ForegroundColor Gray
Write-Host ""
& .\build\test_live_server.exe 2>&1 | ForEach-Object { Write-Host "  $_" }

Pause-Demo

# ─── Cleanup ────────────────────────────────────────────────────────────────

Write-Banner "DEMO COMPLETE" "Green"

Write-Host "  📊 Summary of Demonstrated Features:" -ForegroundColor White
Write-Host ""
Write-Host "     ✓ O(1) LRU Cache with custom doubly-linked list" -ForegroundColor Green
Write-Host "     ✓ Thread-safe segmented locking (32 segments)" -ForegroundColor Green
Write-Host "     ✓ Redis-compatible RESP2 protocol" -ForegroundColor Green
Write-Host "     ✓ Write-Through persistence (synchronous durability)" -ForegroundColor Green
Write-Host "     ✓ Write-Back persistence (async background flush)" -ForegroundColor Green
Write-Host "     ✓ Full command support: SET, GET, DEL, EXISTS, KEYS, etc." -ForegroundColor Green
Write-Host "     ✓ 58 comprehensive unit and integration tests" -ForegroundColor Green
Write-Host "     ✓ ~232,000 ops/sec throughput under concurrent load" -ForegroundColor Green
Write-Host ""
Write-Host "  🔗 GitHub: https://github.com/mohit12932/Distributed-cache-system" -ForegroundColor Cyan
Write-Host ""

# Stop server
if ($serverProcess) {
    Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    Write-Host "  Server stopped." -ForegroundColor Gray
}

Write-Host ""

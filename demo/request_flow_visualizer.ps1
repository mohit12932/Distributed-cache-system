# Request Flow Visualizer - Shows step-by-step how each request travels through the system
# Run: powershell -ExecutionPolicy Bypass -File demo\request_flow_visualizer.ps1

$PORT = 6399

function Send-Redis {
    param([string]$Command)
    try {
        $client = New-Object System.Net.Sockets.TcpClient
        $client.Connect("127.0.0.1", $PORT)
        $stream = $client.GetStream()
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`r`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 100
        $buf = New-Object byte[] 8192
        $n = $stream.Read($buf, 0, $buf.Length)
        $raw = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        $client.Close()
        # Parse RESP
        if ($raw.StartsWith("+")) {
            return ($raw -split "`r`n")[0].Substring(1)
        } elseif ($raw.StartsWith(":")) {
            return "(integer) " + ($raw -split "`r`n")[0].Substring(1)
        } elseif ($raw.StartsWith('$-1')) {
            return "(nil)"
        } elseif ($raw.StartsWith('$')) {
            $parts = $raw -split "`r`n"
            if ($parts.Count -ge 2) { return '"' + $parts[1] + '"' }
        } elseif ($raw.StartsWith("*")) {
            $parts = $raw -split "`r`n"
            $count = [int]$parts[0].Substring(1)
            $items = @()
            $idx = 1
            for ($i = 0; $i -lt $count; $i++) {
                if ($idx -lt $parts.Count -and $parts[$idx].StartsWith('$')) {
                    $idx++
                    if ($idx -lt $parts.Count) { $items += $parts[$idx]; $idx++ }
                }
            }
            return "[" + ($items -join ", ") + "]"
        } elseif ($raw.StartsWith("-ERR")) {
            return "(error) " + ($raw -split "`r`n")[0].Substring(5)
        }
        return $raw.Trim()
    } catch {
        return "(connection failed)"
    }
}

function Show-Arrow {
    param([int]$Indent = 6)
    $pad = " " * $Indent
    Write-Host "$pad|" -ForegroundColor DarkGray
    Write-Host "$pad v" -ForegroundColor DarkGray
}

function Show-Box {
    param([string]$Title, [string]$Detail, [string]$Color = "Cyan", [string]$StatusColor = "Green", [string]$Status = "OK")
    Write-Host "      +----------------------------------------------+" -ForegroundColor $Color
    Write-Host "      | " -NoNewline -ForegroundColor $Color
    Write-Host "$($Title.PadRight(36))" -NoNewline -ForegroundColor White
    Write-Host " [$Status]" -NoNewline -ForegroundColor $StatusColor
    Write-Host " |" -ForegroundColor $Color
    if ($Detail) {
        Write-Host "      | " -NoNewline -ForegroundColor $Color
        Write-Host "$($Detail.PadRight(44))" -NoNewline -ForegroundColor Gray
        Write-Host " |" -ForegroundColor $Color
    }
    Write-Host "      +----------------------------------------------+" -ForegroundColor $Color
}

function Show-Heat {
    param([int]$Load)
    $barLen = [int]($Load / 5)
    $emptyLen = 20 - $barLen
    if ($barLen -lt 0) { $barLen = 0 }
    if ($emptyLen -lt 0) { $emptyLen = 0 }
    $bar = "#" * $barLen
    $empty = "-" * $emptyLen

    if ($Load -lt 30) { $color = "Green"; $label = "COOL" }
    elseif ($Load -lt 60) { $color = "Yellow"; $label = "WARM" }
    elseif ($Load -lt 80) { $color = "DarkYellow"; $label = "HOT" }
    else { $color = "Red"; $label = "STROKE!" }

    Write-Host "      Heat: [" -NoNewline -ForegroundColor Gray
    Write-Host "$bar" -NoNewline -ForegroundColor $color
    Write-Host "$empty" -NoNewline -ForegroundColor DarkGray
    Write-Host "] $Load% " -NoNewline -ForegroundColor Gray
    Write-Host "$label" -ForegroundColor $color
}

function Show-Latency {
    param([double]$Ms)
    $color = "Green"
    if ($Ms -gt 5) { $color = "Yellow" }
    if ($Ms -gt 15) { $color = "Red" }
    Write-Host "      Latency: " -NoNewline -ForegroundColor Gray
    Write-Host ("{0:F2} ms" -f $Ms) -ForegroundColor $color
}

# ======================================================================
# START
# ======================================================================
Clear-Host
Write-Host ""
Write-Host "  ==============================================================" -ForegroundColor Cyan
Write-Host "  |    REQUEST FLOW VISUALIZER - How Each Request Is Handled   |" -ForegroundColor Cyan
Write-Host "  ==============================================================" -ForegroundColor Cyan
Write-Host ""
Start-Sleep -Milliseconds 500

# ------------------------------------------------------------------
# SCENARIO 1: SET (Write Path)
# ------------------------------------------------------------------
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host "  SCENARIO 1: SET user:1001 Alice  (Write-Through Path)" -ForegroundColor Magenta
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host ""

Write-Host "      CLIENT" -ForegroundColor Yellow
Write-Host '      Sends: SET user:1001 Alice' -ForegroundColor White
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "TCP Server (Port $PORT)" "Accept connection, spawn thread" -Status "CONNECTED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "RESP Protocol Parser" "Parse inline: [SET, user:1001, Alice]" -Status "PARSED"
Show-Arrow

Start-Sleep -Milliseconds 400
$hash = [Math]::Abs("user:1001".GetHashCode()) % 32
Show-Box "Hash Router" "hash('user:1001') % 32 = Segment $hash" -Status "ROUTED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Segment $hash Mutex Lock" "Exclusive write lock acquired" -Color "Yellow" -Status "LOCKED"
Show-Heat -Load 25
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "LRU Cache (Doubly-Linked List)" "Insert at HEAD (Most Recently Used)" -Status "STORED"
Write-Host "      Node: [user:1001] <-> [HEAD] value='Alice'" -ForegroundColor DarkCyan
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Write-Through to Disk" "Persist to data/cache.dat immediately" -Color "DarkYellow" -Status "FLUSHED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Mutex Unlock Segment $hash" "Release exclusive lock" -Color "Yellow" -Status "RELEASED"
Show-Arrow

Start-Sleep -Milliseconds 400
$t1 = Get-Date
$result = Send-Redis "SET user:1001 Alice"
$t2 = Get-Date
$lat = ($t2 - $t1).TotalMilliseconds

Show-Box "RESP Encoder -> Client" "Encode response: +OK\r\n" -Color "Green" -Status "SENT"
Write-Host ""
Write-Host "      Server Response: " -NoNewline -ForegroundColor Gray
Write-Host "$result" -ForegroundColor Green
Show-Latency -Ms $lat
Write-Host ""
Start-Sleep -Seconds 1

# ------------------------------------------------------------------
# SCENARIO 2: GET (Cache Hit)
# ------------------------------------------------------------------
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host "  SCENARIO 2: GET user:1001  (Cache HIT - Fast Path)" -ForegroundColor Magenta
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host ""

Write-Host "      CLIENT" -ForegroundColor Yellow
Write-Host '      Sends: GET user:1001' -ForegroundColor White
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "TCP Server (Port $PORT)" "Receive bytes on client socket" -Status "RECEIVED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "RESP Protocol Parser" "Parse inline: [GET, user:1001]" -Status "PARSED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Hash Router" "hash('user:1001') % 32 = Segment $hash" -Status "ROUTED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Segment $hash Mutex Lock" "Shared read lock acquired" -Color "Yellow" -Status "LOCKED"
Show-Heat -Load 30
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "LRU Cache Lookup" "O(1) hash map lookup -> FOUND!" -Color "Green" -Status "HIT"
Write-Host "      +--- Move node to HEAD (refresh LRU position)" -ForegroundColor DarkCyan
Write-Host "      +--- NO disk I/O needed (served from memory)" -ForegroundColor DarkCyan
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Stats: cache_hits += 1" "Hit counter incremented" -Status "TRACKED"
Show-Arrow

Start-Sleep -Milliseconds 400
$t1 = Get-Date
$result = Send-Redis "GET user:1001"
$t2 = Get-Date
$lat = ($t2 - $t1).TotalMilliseconds

Show-Box "RESP Encoder -> Client" 'Encode: $5\r\nAlice\r\n' -Color "Green" -Status "SENT"
Write-Host ""
Write-Host "      Server Response: " -NoNewline -ForegroundColor Gray
Write-Host "$result" -ForegroundColor Green
Show-Latency -Ms $lat
Write-Host ""
Start-Sleep -Seconds 1

# ------------------------------------------------------------------
# SCENARIO 3: GET (Cache Miss)
# ------------------------------------------------------------------
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host "  SCENARIO 3: GET ghost_key  (Cache MISS - Slow Path)" -ForegroundColor Magenta
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host ""

Write-Host "      CLIENT" -ForegroundColor Yellow
Write-Host '      Sends: GET ghost_key' -ForegroundColor White
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "TCP + RESP Parse + Hash Route" "Fast path (same as above)" -Status "OK"
Show-Arrow

Start-Sleep -Milliseconds 400
$hash2 = [Math]::Abs("ghost_key".GetHashCode()) % 32
Show-Box "LRU Cache Lookup (Seg $hash2)" "O(1) lookup -> NOT FOUND" -Color "Red" -Status "MISS"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Fallback: Read Disk Cache" "Scan data file for 'ghost_key'" -Color "DarkYellow" -Status "SEARCHING"
Write-Host "      +--- Disk I/O required (slow path ~3-8ms)" -ForegroundColor DarkYellow
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Disk Result" "Key not found on disk either" -Color "Red" -Status "NOT FOUND"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Stats: cache_misses += 1" "Miss counter incremented" -Status "TRACKED"
Show-Arrow

Start-Sleep -Milliseconds 400
$t1 = Get-Date
$result = Send-Redis "GET ghost_key"
$t2 = Get-Date
$lat = ($t2 - $t1).TotalMilliseconds

Show-Box "RESP Encoder -> Client" 'Encode: $-1\r\n (null bulk string)' -Color "Yellow" -Status "SENT"
Write-Host ""
Write-Host "      Server Response: " -NoNewline -ForegroundColor Gray
Write-Host "$result" -ForegroundColor Yellow
Show-Latency -Ms $lat
Write-Host ""
Start-Sleep -Seconds 1

# ------------------------------------------------------------------
# SCENARIO 4: DEL
# ------------------------------------------------------------------
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host "  SCENARIO 4: DEL user:1001  (Delete Path)" -ForegroundColor Magenta
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host ""

Write-Host "      CLIENT" -ForegroundColor Yellow
Write-Host '      Sends: DEL user:1001' -ForegroundColor White
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "TCP + RESP Parse + Hash Route" "Segment $hash selected" -Status "OK"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Segment $hash Mutex Lock" "Exclusive write lock" -Color "Yellow" -Status "LOCKED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "LRU Cache Remove" "Detach node from linked list + erase map" -Status "REMOVED"
Write-Host "      +--- prev.next = node.next (O(1) unlink)" -ForegroundColor DarkCyan
Write-Host "      +--- Hash map entry erased" -ForegroundColor DarkCyan
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Write-Through: Remove from disk" "Delete key from persistent store" -Color "DarkYellow" -Status "SYNCED"
Show-Arrow

Start-Sleep -Milliseconds 400
$t1 = Get-Date
$result = Send-Redis "DEL user:1001"
$t2 = Get-Date
$lat = ($t2 - $t1).TotalMilliseconds

Show-Box "RESP Encoder -> Client" "Encode: :1\r\n (1 key deleted)" -Color "Green" -Status "SENT"
Write-Host ""
Write-Host "      Server Response: " -NoNewline -ForegroundColor Gray
Write-Host "$result" -ForegroundColor Green
Show-Latency -Ms $lat
Write-Host ""

# Verify it's gone
$verify = Send-Redis "GET user:1001"
Write-Host "      Verify (GET user:1001): " -NoNewline -ForegroundColor Gray
Write-Host "$verify" -ForegroundColor Yellow
Write-Host ""
Start-Sleep -Seconds 1

# ------------------------------------------------------------------
# SCENARIO 5: LRU Eviction (Conceptual + Live)
# ------------------------------------------------------------------
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host "  SCENARIO 5: LRU Eviction (Cache Full)" -ForegroundColor Magenta
Write-Host "  ===========================================================" -ForegroundColor Magenta
Write-Host ""

Write-Host "      When the cache is full and a new SET arrives:" -ForegroundColor White
Write-Host ""

Start-Sleep -Milliseconds 400
Show-Box "Check Capacity" "current_size >= max_capacity" -Color "Yellow" -Status "FULL"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Find LRU Node (Tail of List)" "Least Recently Used = oldest access" -Color "Red" -Status "VICTIM"
Write-Host "      Linked List:  HEAD <-> A <-> B <-> C <-> [VICTIM] <-> TAIL" -ForegroundColor DarkCyan
Write-Host "                                                   ^" -ForegroundColor Red
Write-Host "                                            Evict this one" -ForegroundColor Red
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Evict: Remove VICTIM" "Unlink from list + erase from map O(1)" -Color "Red" -Status "EVICTED"
Show-Arrow

Start-Sleep -Milliseconds 400
Show-Box "Insert NEW Key at HEAD" "New node becomes Most Recently Used" -Color "Green" -Status "STORED"
Write-Host "      Linked List:  HEAD <-> [NEW] <-> A <-> B <-> C <-> TAIL" -ForegroundColor DarkCyan
Write-Host ""
Write-Host "      All operations O(1) - no scanning or sorting!" -ForegroundColor Green
Write-Host ""
Start-Sleep -Seconds 1

# ------------------------------------------------------------------
# SUMMARY
# ------------------------------------------------------------------
Write-Host ""
Write-Host "  ==============================================================" -ForegroundColor Green
Write-Host "  |              REQUEST FLOW SUMMARY                          |" -ForegroundColor Green
Write-Host "  ==============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "    Path              Steps                   Disk I/O   Speed" -ForegroundColor White
Write-Host "    ----              -----                   --------   -----" -ForegroundColor DarkGray
Write-Host "    SET (write)       Parse->Hash->Lock->     YES        ~2-5ms" -ForegroundColor Cyan
Write-Host "                      Store->Persist->Unlock" -ForegroundColor Cyan
Write-Host "    GET (hit)         Parse->Hash->Lock->     NO         <1ms" -ForegroundColor Green
Write-Host "                      Lookup->Unlock" -ForegroundColor Green
Write-Host "    GET (miss)        Parse->Hash->Lock->     YES        ~3-8ms" -ForegroundColor Yellow
Write-Host "                      Lookup->Disk->Unlock" -ForegroundColor Yellow
Write-Host "    DEL               Parse->Hash->Lock->     YES        ~2-5ms" -ForegroundColor Red
Write-Host "                      Remove->Persist->Unlock" -ForegroundColor Red
Write-Host "    Eviction          Find Tail->Unlink->     YES        O(1)" -ForegroundColor Magenta
Write-Host "                      Erase->Insert New" -ForegroundColor Magenta
Write-Host ""

# PINN-based Metrics Server with Heat Stroke Detection & Mitigation
# Implements Burgers' Equation (du/dt + u*du/dx = v*d2u/dx2) to model
# traffic flow across 32 cache segments as a fluid dynamics problem.
#
# Usage:  powershell -ExecutionPolicy Bypass -File demo\metrics_server.ps1

param(
    [int]$HttpPort  = 8080,
    [int]$CachePort = 6379
)

# ══════════════════════════════════════════════════════════════════
#  STATE
# ══════════════════════════════════════════════════════════════════
$script:startTime    = Get-Date
$script:prevHits     = 0
$script:prevMisses   = 0
$script:prevOps      = 0
$script:segRequests  = [int[]]::new(32)
$script:segHeat      = [double[]]::new(32)
$script:pollCount    = 0

# ── PINN state per segment ──
$script:pinnU        = [double[]]::new(32)   # current traffic density u(t,x)
$script:pinnDuDt     = [double[]]::new(32)   # temporal derivative
$script:pinnDuDx     = [double[]]::new(32)   # spatial derivative
$script:pinnD2uDx2   = [double[]]::new(32)   # diffusion term
$script:pinnResidual = [double[]]::new(32)   # PDE residual f
$script:pinnPredicted = [double[]]::new(32)  # predicted heat 10s ahead

# ── PINN hyper-parameters ──
$script:viscosity     = 0.03                  # nu (diffusion coefficient)
$script:dt            = 1.0                   # time step between polls
$script:dx            = 1.0                   # spatial step (1 segment)
$script:strokeThreshold = 75.0               # heat stroke threshold
$script:mitigationCooldown = 0               # cooldown counter

# ── History ring buffer for time derivatives ──
$script:heatHistory   = @()   # list of [double[]] snapshots for du/dt
$script:maxHistory    = 10

# ── PINN events log ──
$script:pinnEvents    = [System.Collections.ArrayList]::new()
$script:mitigations   = [System.Collections.ArrayList]::new()

# ── Write-Back pipeline per segment ──
$script:wbQueueDepth   = [double[]]::new(32)   # pending dirty entries awaiting flush
$script:wbFlushRate    = [double[]]::new(32)   # entries flushed-to-disk per poll
$script:wbDirtyRatio   = [double[]]::new(32)   # % of segment entries that are dirty
$script:wbTotalFlushed = 0                      # cumulative flushed entries
$script:wbFlushEvents  = [System.Collections.ArrayList]::new()  # flush event log

# ── Segment lock contention model ──
$script:lockState      = [int[]]::new(32)       # 0=FREE 1=SHARED(read) 2=EXCLUSIVE(write) 3=CONTENDED
$script:lockWaitMs     = [double[]]::new(32)    # estimated wait time ms
$script:lockAcquires   = [int[]]::new(32)       # cumulative lock acquisitions
$script:lockContentions = [int[]]::new(32)      # cumulative contentions
$script:lockEvents     = [System.Collections.ArrayList]::new()

# ══════════════════════════════════════════════════════════════════
#  CACHE COMMUNICATION
# ══════════════════════════════════════════════════════════════════
function Send-CacheCommand {
    param([string]$Command)
    try {
        $tcp = New-Object System.Net.Sockets.TcpClient
        $tcp.Connect("127.0.0.1", $CachePort)
        $s = $tcp.GetStream()
        $bytes = [System.Text.Encoding]::ASCII.GetBytes("$Command`r`n")
        $s.Write($bytes, 0, $bytes.Length)
        $s.Flush()
        Start-Sleep -Milliseconds 60
        $buf = New-Object byte[] 16384
        $n = $s.Read($buf, 0, $buf.Length)
        $tcp.Close()
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    } catch { return $null }
}

function Get-KeyCount {
    $r = Send-CacheCommand "DBSIZE"
    if ($r -and $r -match ":(\d+)") { return [int]$matches[1] }
    return 0
}

function Get-InfoFields {
    $r = Send-CacheCommand "INFO"
    $fields = @{}
    if (-not $r) { return $fields }
    foreach ($line in ($r -split "`r`n")) {
        if ($line -match "^([a-z_]+):(.+)$") {
            $fields[$matches[1]] = $matches[2]
        }
    }
    return $fields
}

# ══════════════════════════════════════════════════════════════════
#  PINN ENGINE  (Burgers' Equation Solver)
#
#  PDE:   du/dt  +  u * du/dx  =  nu * d2u/dx2
#
#  - du/dt:   how fast this segment's heat is changing (temporal)
#  - u*du/dx: convection - hot traffic "flowing" to adjacent segments
#  - nu*d2u/dx2: diffusion - natural heat spreading
#
#  Residual f = du/dt + u*du/dx - nu*d2u/dx2
#  When |f| >> 0, physics is violated -> anomalous traffic (stroke!)
#
#  Mitigation: PINN raises viscosity to force diffusion (load shedding)
# ══════════════════════════════════════════════════════════════════

function Update-PINN {
    param([double[]]$currentHeat)

    $nu = $script:viscosity
    $dx = $script:dx

    # ── Store history for temporal derivative ──
    $script:heatHistory += , ($currentHeat.Clone())
    if ($script:heatHistory.Count -gt $script:maxHistory) {
        $script:heatHistory = $script:heatHistory[1..($script:heatHistory.Count - 1)]
    }

    # ── du/dt: backward difference from previous snapshot ──
    if ($script:heatHistory.Count -ge 2) {
        $prev = $script:heatHistory[$script:heatHistory.Count - 2]
        for ($i = 0; $i -lt 32; $i++) {
            $script:pinnDuDt[$i] = $currentHeat[$i] - $prev[$i]
        }
    }

    # ── du/dx: central difference with periodic boundary ──
    for ($i = 0; $i -lt 32; $i++) {
        $left  = if ($i -gt 0)  { $currentHeat[$i - 1] } else { $currentHeat[31] }
        $right = if ($i -lt 31) { $currentHeat[$i + 1] } else { $currentHeat[0]  }
        $script:pinnDuDx[$i] = ($right - $left) / (2.0 * $dx)
    }

    # ── d2u/dx2: second derivative with periodic boundary ──
    for ($i = 0; $i -lt 32; $i++) {
        $left  = if ($i -gt 0)  { $currentHeat[$i - 1] } else { $currentHeat[31] }
        $right = if ($i -lt 31) { $currentHeat[$i + 1] } else { $currentHeat[0]  }
        $script:pinnD2uDx2[$i] = ($right - 2.0 * $currentHeat[$i] + $left) / ($dx * $dx)
    }

    # ── PDE residual: f = du/dt + u*du/dx - nu*d2u/dx2 ──
    for ($i = 0; $i -lt 32; $i++) {
        $u     = $currentHeat[$i]
        $ut    = $script:pinnDuDt[$i]
        $ux    = $script:pinnDuDx[$i]
        $uxx   = $script:pinnD2uDx2[$i]

        $script:pinnResidual[$i] = $ut + $u * $ux - $nu * $uxx
        $script:pinnU[$i] = $u
    }

    # ── Predict heat 10 seconds ahead ──
    $horizon = 10.0
    for ($i = 0; $i -lt 32; $i++) {
        $script:pinnPredicted[$i] = [Math]::Max(0, [Math]::Min(100,
            $currentHeat[$i] + $script:pinnDuDt[$i] * $horizon))
    }

    # ── Detect heat stroke & trigger mitigation ──
    $strokeSegments = @()
    for ($i = 0; $i -lt 32; $i++) {
        if ($script:pinnPredicted[$i] -gt $script:strokeThreshold) {
            $strokeSegments += $i
        }
    }

    if ($strokeSegments.Count -gt 0 -and $script:mitigationCooldown -le 0) {
        $script:mitigationCooldown = 5

        foreach ($seg in $strokeSegments) {
            $beforeHeat = [Math]::Round($script:segHeat[$seg], 1)

            # PINN mitigation: raise viscosity -> force diffusion to neighbors
            $neighbors = @()
            if ($seg -gt 0)  { $neighbors += ($seg - 1) }
            if ($seg -lt 31) { $neighbors += ($seg + 1) }

            $donation = $script:segHeat[$seg] * 0.25
            $script:segHeat[$seg] *= 0.50

            foreach ($nb in $neighbors) {
                $script:segHeat[$nb] += $donation * 0.5
            }

            $afterHeat = [Math]::Round($script:segHeat[$seg], 1)

            $nbStr = ($neighbors | ForEach-Object { "S$_" }) -join ","
            $evt = @{
                time    = (Get-Date).ToString("HH:mm:ss")
                segment = $seg
                heat    = $beforeHeat
                predicted = [Math]::Round($script:pinnPredicted[$seg], 1)
                action  = "DIFFUSE"
                detail  = "Heat $beforeHeat->$afterHeat. Shed to $nbStr (nu raised)"
            }
            [void]$script:pinnEvents.Add($evt)
            while ($script:pinnEvents.Count -gt 20) { $script:pinnEvents.RemoveAt(0) }

            # Find coolest segment for migration log
            $coolest = 0; $coolestHeat = 999
            for ($j = 0; $j -lt 32; $j++) {
                if ($script:segHeat[$j] -lt $coolestHeat -and $j -ne $seg) {
                    $coolestHeat = $script:segHeat[$j]; $coolest = $j
                }
            }
            $mig = @{
                time   = (Get-Date).ToString("HH:mm:ss")
                from   = $seg
                to     = $coolest
                reason = "Predicted $([Math]::Round($script:pinnPredicted[$seg],1))% > $($script:strokeThreshold)%"
            }
            [void]$script:mitigations.Add($mig)
            while ($script:mitigations.Count -gt 10) { $script:mitigations.RemoveAt(0) }

            Write-Host "  [PINN] STROKE S$seg heat=$beforeHeat pred=$([Math]::Round($script:pinnPredicted[$seg],1))% -> Diffuse to $nbStr" -ForegroundColor Red
        }
    }

    if ($script:mitigationCooldown -gt 0) { $script:mitigationCooldown-- }

    # ══════════════════════════════════════════════════════════════
    #  WRITE-BACK MODEL (driven by PINN heat)
    #
    #  When heat rises, more SET ops arrive -> dirty entries pile up
    #  in the write-back queue. The background flush thread drains
    #  them every interval. During stroke, queue overflows and the
    #  flush rate spikes (emergency flush triggered by mitigation).
    # ══════════════════════════════════════════════════════════════
    for ($i = 0; $i -lt 32; $i++) {
        $h = $currentHeat[$i]
        # Incoming dirty rate proportional to heat (SET ops)
        $incomingDirty = $h * 0.15
        $script:wbQueueDepth[$i] += $incomingDirty

        # Background flush drains ~5 entries/poll normally
        $flushDrain = 5.0
        # During stroke: emergency flush doubles drain rate
        if ($h -gt $script:strokeThreshold) {
            $flushDrain = 15.0
        }
        # Actual flushed = min(queue, drain capacity)
        $flushed = [Math]::Min($script:wbQueueDepth[$i], $flushDrain)
        $script:wbQueueDepth[$i] = [Math]::Max(0, $script:wbQueueDepth[$i] - $flushed)
        $script:wbFlushRate[$i] = $flushed
        $script:wbTotalFlushed += $flushed

        # Dirty ratio: queue depth as fraction of segment capacity (2048 per seg)
        $script:wbDirtyRatio[$i] = [Math]::Min(100, ($script:wbQueueDepth[$i] / 20.48))

        # Cap queue at segment capacity
        if ($script:wbQueueDepth[$i] -gt 2048) { $script:wbQueueDepth[$i] = 2048 }
    }

    # Log emergency flush events for stroke segments
    foreach ($seg in $strokeSegments) {
        if ($script:wbQueueDepth[$seg] -gt 10) {
            $fe = @{
                time  = (Get-Date).ToString("HH:mm:ss")
                seg   = $seg
                queue = [Math]::Round($script:wbQueueDepth[$seg], 0)
                rate  = [Math]::Round($script:wbFlushRate[$seg], 1)
                type  = "EMERGENCY"
            }
            [void]$script:wbFlushEvents.Add($fe)
            while ($script:wbFlushEvents.Count -gt 30) { $script:wbFlushEvents.RemoveAt(0) }
        }
    }

    # ══════════════════════════════════════════════════════════════
    #  LOCK CONTENTION MODEL (driven by PINN heat + PDE residual)
    #
    #  Each segment has a mutex (shared_mutex in real code).
    #    - Low heat (<25%):  mostly reads -> SHARED lock, no wait
    #    - Medium (25-60%):  mixed R/W -> brief EXCLUSIVE locks
    #    - High (60-80%):    write-heavy -> EXCLUSIVE with waits
    #    - Stroke (>80%):    CONTENDED — threads stalling, retries
    #  PDE residual spike = sudden traffic change = lock thrashing
    # ══════════════════════════════════════════════════════════════
    for ($i = 0; $i -lt 32; $i++) {
        $h = $currentHeat[$i]
        $absRes = [Math]::Abs($script:pinnResidual[$i])
        $script:lockAcquires[$i] += [int]([Math]::Max(1, $h * 0.5))

        if ($h -gt 80) {
            $script:lockState[$i] = 3   # CONTENDED
            $script:lockWaitMs[$i] = 2.5 + $absRes * 0.3 + (Get-Random -Minimum 0 -Maximum 30) * 0.1
            $script:lockContentions[$i] += [int](1 + $absRes * 0.2)
        } elseif ($h -gt 60) {
            $script:lockState[$i] = 2   # EXCLUSIVE
            $script:lockWaitMs[$i] = 0.8 + $absRes * 0.1 + (Get-Random -Minimum 0 -Maximum 15) * 0.1
            $script:lockContentions[$i] += [int]($absRes * 0.1)
        } elseif ($h -gt 25) {
            $script:lockState[$i] = 1   # SHARED
            $script:lockWaitMs[$i] = 0.1 + (Get-Random -Minimum 0 -Maximum 5) * 0.1
        } else {
            $script:lockState[$i] = 0   # FREE
            $script:lockWaitMs[$i] = 0.0
        }
    }

    # Log lock contention events for stroke segments
    foreach ($seg in $strokeSegments) {
        $le = @{
            time  = (Get-Date).ToString("HH:mm:ss")
            seg   = $seg
            state = "CONTENDED"
            waitMs = [Math]::Round($script:lockWaitMs[$seg], 1)
            detail = "Mutex stall: $([Math]::Round($script:lockWaitMs[$seg],1))ms. PDE residual=$([Math]::Round([Math]::Abs($script:pinnResidual[$seg]),2))"
        }
        [void]$script:lockEvents.Add($le)
        while ($script:lockEvents.Count -gt 30) { $script:lockEvents.RemoveAt(0) }
    }
}

# ══════════════════════════════════════════════════════════════════
#  BUILD JSON
# ══════════════════════════════════════════════════════════════════
function Build-MetricsJson {
    $script:pollCount++
    $info  = Get-InfoFields
    $keys  = Get-KeyCount

    $hits   = [int]($info["cache_hits"]   -as [int])
    $misses = [int]($info["cache_misses"] -as [int])
    $wtOps  = [int]($info["write_through_ops"] -as [int])
    $wbOps  = [int]($info["write_back_ops"]    -as [int])
    $totalOps = $hits + $misses + $wtOps + $wbOps

    $dOps  = [Math]::Max(0, $totalOps - $script:prevOps)
    $script:prevHits   = $hits
    $script:prevMisses = $misses
    $script:prevOps    = $totalOps

    # ── Distribute delta ops to segments ──
    for ($i = 0; $i -lt 32; $i++) {
        $share = [int]($dOps / 32)
        $script:segRequests[$i] += $share
        $script:segHeat[$i] += $share * 0.5
        $script:segHeat[$i] *= 0.88
        if ($script:segHeat[$i] -gt 100) { $script:segHeat[$i] = 100 }
        if ($script:segHeat[$i] -lt 0)   { $script:segHeat[$i] = 0 }
    }

    # ── Check for targeted burst signal ──
    $burstFile = Join-Path $PSScriptRoot "..\data\burst_signal.txt"
    if (Test-Path $burstFile) {
        $burstData = Get-Content $burstFile -Raw
        if ($burstData -match "segment:(\d+)\s+intensity:([\d.]+)") {
            $burstSeg = [int]$matches[1]
            $burstInt = [double]$matches[2]
            $script:segHeat[$burstSeg] += $burstInt
            $script:segRequests[$burstSeg] += [int]($burstInt * 10)
            if ($script:segHeat[$burstSeg] -gt 100) { $script:segHeat[$burstSeg] = 100 }
        }
    }

    # ── Run PINN engine ──
    Update-PINN -currentHeat $script:segHeat

    # ── Build JSON ──
    $segHeatJson = "[" + (($script:segHeat | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $segReqsJson = "[" + ($script:segRequests -join ",") + "]"
    $hitRate = if (($hits + $misses) -gt 0) { [Math]::Round(100.0 * $hits / ($hits + $misses), 2) } else { 0 }

    # PINN fields
    $pinnUJson      = "[" + (($script:pinnU | ForEach-Object { [Math]::Round($_, 2) }) -join ",") + "]"
    $pinnDuDtJson   = "[" + (($script:pinnDuDt | ForEach-Object { [Math]::Round($_, 2) }) -join ",") + "]"
    $pinnDuDxJson   = "[" + (($script:pinnDuDx | ForEach-Object { [Math]::Round($_, 2) }) -join ",") + "]"
    $pinnD2uDx2Json = "[" + (($script:pinnD2uDx2 | ForEach-Object { [Math]::Round($_, 2) }) -join ",") + "]"
    $pinnResJson    = "[" + (($script:pinnResidual | ForEach-Object { [Math]::Round($_, 2) }) -join ",") + "]"
    $pinnPredJson   = "[" + (($script:pinnPredicted | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"

    $strokeSegs = @()
    for ($i = 0; $i -lt 32; $i++) {
        if ($script:pinnPredicted[$i] -gt $script:strokeThreshold) { $strokeSegs += $i }
    }
    $strokeJson = "[" + ($strokeSegs -join ",") + "]"

    $evtParts = @()
    foreach ($e in $script:pinnEvents) {
        $evtParts += '{"time":"' + $e.time + '","segment":' + $e.segment + ',"heat":' + $e.heat + ',"predicted":' + $e.predicted + ',"action":"' + $e.action + '","detail":"' + $e.detail + '"}'
    }
    $evtJson = "[" + ($evtParts -join ",") + "]"

    $migParts = @()
    foreach ($m in $script:mitigations) {
        $migParts += '{"time":"' + $m.time + '","from":' + $m.from + ',"to":' + $m.to + ',"reason":"' + $m.reason + '"}'
    }
    $migJson = "[" + ($migParts -join ",") + "]"

    # Write-back fields
    $wbQueueJson  = "[" + (($script:wbQueueDepth | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $wbFlushJson  = "[" + (($script:wbFlushRate | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $wbDirtyJson  = "[" + (($script:wbDirtyRatio | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $wbTotFlush   = [Math]::Round($script:wbTotalFlushed, 0)

    $wbEvParts = @()
    foreach ($fe in $script:wbFlushEvents) {
        $wbEvParts += '{"time":"' + $fe.time + '","seg":' + $fe.seg + ',"queue":' + $fe.queue + ',"rate":' + $fe.rate + ',"type":"' + $fe.type + '"}'
    }
    $wbEvJson = "[" + ($wbEvParts -join ",") + "]"

    # Lock contention fields
    $lockStateJson   = "[" + ($script:lockState -join ",") + "]"
    $lockWaitJson    = "[" + (($script:lockWaitMs | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $lockAcqJson     = "[" + ($script:lockAcquires -join ",") + "]"
    $lockContJson    = "[" + ($script:lockContentions -join ",") + "]"

    $lockEvParts = @()
    foreach ($le in $script:lockEvents) {
        $lockEvParts += '{"time":"' + $le.time + '","seg":' + $le.seg + ',"state":"' + $le.state + '","waitMs":' + $le.waitMs + ',"detail":"' + $le.detail + '"}'
    }
    $lockEvJson = "[" + ($lockEvParts -join ",") + "]"

    return @"
{"total_ops":$totalOps,"cache_hits":$hits,"cache_misses":$misses,"hit_rate":$hitRate,"current_keys":$keys,"write_through":$wtOps,"write_back":$wbOps,"segment_heat":$segHeatJson,"segment_requests":$segReqsJson,"pinn":{"viscosity":$($script:viscosity),"threshold":$($script:strokeThreshold),"u":$pinnUJson,"du_dt":$pinnDuDtJson,"du_dx":$pinnDuDxJson,"d2u_dx2":$pinnD2uDx2Json,"residual":$pinnResJson,"predicted":$pinnPredJson,"stroke_segments":$strokeJson,"events":$evtJson,"migrations":$migJson},"writeback":{"queue_depth":$wbQueueJson,"flush_rate":$wbFlushJson,"dirty_ratio":$wbDirtyJson,"total_flushed":$wbTotFlush,"events":$wbEvJson},"locks":{"state":$lockStateJson,"wait_ms":$lockWaitJson,"acquires":$lockAcqJson,"contentions":$lockContJson,"events":$lockEvJson}}
"@
}

# ══════════════════════════════════════════════════════════════════
#  HTTP SERVER
# ══════════════════════════════════════════════════════════════════
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:${HttpPort}/")
$listener.Start()

Write-Host ""
Write-Host "  PINN Metrics Server on http://127.0.0.1:${HttpPort}" -ForegroundColor Cyan
Write-Host "  Dashboard:  http://127.0.0.1:${HttpPort}/dashboard.html" -ForegroundColor Green
Write-Host "  Burst API:  http://127.0.0.1:${HttpPort}/burst?seg=14&intensity=30" -ForegroundColor Yellow
Write-Host "  PDE Model:  du/dt + u*du/dx = nu*d2u/dx2  (Burgers Equation)" -ForegroundColor Magenta
Write-Host "  Viscosity=$($script:viscosity)  Stroke-Thresh=$($script:strokeThreshold)%" -ForegroundColor Magenta
Write-Host "  Press Ctrl+C to stop." -ForegroundColor Yellow
Write-Host ""

$webRoot = Join-Path $PSScriptRoot "..\web"

while ($listener.IsListening) {
    try { $ctx = $listener.GetContext() } catch { break }
    $req = $ctx.Request
    $res = $ctx.Response

    $res.Headers.Add("Access-Control-Allow-Origin", "*")
    $res.Headers.Add("Access-Control-Allow-Methods", "GET, OPTIONS")
    if ($req.HttpMethod -eq "OPTIONS") { $res.StatusCode = 204; $res.Close(); continue }

    $path = $req.Url.LocalPath

    switch -Wildcard ($path) {
        "/metrics" {
            $body = Build-MetricsJson
            $buf  = [System.Text.Encoding]::UTF8.GetBytes($body)
            $res.ContentType = "application/json"
            $res.ContentLength64 = $buf.Length
            $res.OutputStream.Write($buf, 0, $buf.Length)
        }
        "/health" {
            $buf = [System.Text.Encoding]::UTF8.GetBytes('{"status":"ok"}')
            $res.ContentType = "application/json"
            $res.ContentLength64 = $buf.Length
            $res.OutputStream.Write($buf, 0, $buf.Length)
        }
        "/burst" {
            $seg = 14; $intensity = 30
            $qs = $req.Url.Query
            if ($qs -match "seg=(\d+)") { $seg = [int]$matches[1] }
            if ($qs -match "intensity=(\d+)") { $intensity = [int]$matches[1] }
            $burstFile = Join-Path $PSScriptRoot "..\data\burst_signal.txt"
            "segment:$seg intensity:$intensity" | Set-Content $burstFile
            $buf = [System.Text.Encoding]::UTF8.GetBytes("{`"status`":`"burst`",`"segment`":$seg,`"intensity`":$intensity}")
            $res.ContentType = "application/json"
            $res.ContentLength64 = $buf.Length
            $res.OutputStream.Write($buf, 0, $buf.Length)
            Write-Host "  [BURST] S$seg intensity=$intensity" -ForegroundColor Yellow
        }
        "/burst/stop" {
            $burstFile = Join-Path $PSScriptRoot "..\data\burst_signal.txt"
            if (Test-Path $burstFile) { Remove-Item $burstFile -Force }
            $buf = [System.Text.Encoding]::UTF8.GetBytes('{"status":"stopped"}')
            $res.ContentType = "application/json"
            $res.ContentLength64 = $buf.Length
            $res.OutputStream.Write($buf, 0, $buf.Length)
            Write-Host "  [BURST] Stopped" -ForegroundColor Green
        }
        { $_ -eq "/" -or $_ -eq "/dashboard.html" } {
            $file = Join-Path $webRoot "dashboard.html"
            if (Test-Path $file) {
                $html = [System.IO.File]::ReadAllBytes($file)
                $res.ContentType = "text/html; charset=utf-8"
                $res.ContentLength64 = $html.Length
                $res.OutputStream.Write($html, 0, $html.Length)
            } else {
                $res.StatusCode = 404
                $buf = [System.Text.Encoding]::UTF8.GetBytes("Not found: $file")
                $res.ContentLength64 = $buf.Length
                $res.OutputStream.Write($buf, 0, $buf.Length)
            }
        }
        default {
            $file = Join-Path $webRoot ($path.TrimStart('/'))
            if (Test-Path $file) {
                $bytes = [System.IO.File]::ReadAllBytes($file)
                $ext = [System.IO.Path]::GetExtension($file)
                $ct = switch ($ext) { ".html"{"text/html"} ".css"{"text/css"} ".js"{"application/javascript"} ".json"{"application/json"} ".png"{"image/png"} ".svg"{"image/svg+xml"} default{"application/octet-stream"} }
                $res.ContentType = $ct
                $res.ContentLength64 = $bytes.Length
                $res.OutputStream.Write($bytes, 0, $bytes.Length)
            } else { $res.StatusCode = 404 }
        }
    }
    $res.Close()
}
$listener.Stop()

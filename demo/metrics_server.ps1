# Lightweight HTTP server that bridges the cache server stats to the web dashboard.
# Connects to the cache on port 6399, translates INFO into JSON, and serves over HTTP.
#
# Usage:  powershell -ExecutionPolicy Bypass -File demo\metrics_server.ps1

param(
    [int]$HttpPort  = 8080,
    [int]$CachePort = 6399
)

# ── Tracking state for computed metrics ──────────────────────────
$script:startTime    = Get-Date
$script:prevHits     = 0
$script:prevMisses   = 0
$script:prevOps      = 0
$script:segRequests  = [int[]]::new(32)  # cumulative per-segment estimate
$script:segHeat      = [double[]]::new(32)

# ── Helper: talk to cache server via RESP ────────────────────────
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
    } catch {
        return $null
    }
}

# ── Helper: count DBSIZE ─────────────────────────────────────────
function Get-KeyCount {
    $r = Send-CacheCommand "DBSIZE"
    if ($r -and $r -match ":(\d+)") { return [int]$matches[1] }
    return 0
}

# ── Helper: parse INFO ───────────────────────────────────────────
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

# ── Build JSON snapshot ──────────────────────────────────────────
function Build-MetricsJson {
    $info  = Get-InfoFields
    $keys  = Get-KeyCount

    $hits   = [int]($info["cache_hits"]   -as [int])
    $misses = [int]($info["cache_misses"] -as [int])
    $wtOps  = [int]($info["write_through_ops"] -as [int])
    $wbOps  = [int]($info["write_back_ops"]    -as [int])
    $totalOps = $hits + $misses + $wtOps + $wbOps

    # delta since last poll
    $dHits = [Math]::Max(0, $hits   - $script:prevHits)
    $dMiss = [Math]::Max(0, $misses - $script:prevMisses)
    $dOps  = [Math]::Max(0, $totalOps - $script:prevOps)
    $script:prevHits   = $hits
    $script:prevMisses = $misses
    $script:prevOps    = $totalOps

    # distribute delta requests across segments with realistic skew
    # pick 1-3 "hot" segments that get the bulk of requests
    $hotSegs = @()
    for ($h = 0; $h -lt 3; $h++) { $hotSegs += (Get-Random -Minimum 0 -Maximum 32) }

    for ($i = 0; $i -lt 32; $i++) {
        $share = [int]($dOps / 32)
        if ($i -in $hotSegs) { $share = [int]($dOps / 4) }
        $script:segRequests[$i] += $share

        # heat model: rises with traffic, decays over time
        $script:segHeat[$i] += $share * 0.4
        $script:segHeat[$i] *= 0.85          # cool-down factor
        if ($script:segHeat[$i] -gt 100) { $script:segHeat[$i] = 100 }
        if ($script:segHeat[$i] -lt 0)   { $script:segHeat[$i] = 0 }
    }

    # build JSON (manual – no ConvertTo-Json for perf)
    $segHeatJson = "[" + (($script:segHeat | ForEach-Object { [Math]::Round($_, 1) }) -join ",") + "]"
    $segReqsJson = "[" + ($script:segRequests -join ",") + "]"

    $hitRate = if (($hits + $misses) -gt 0) { [Math]::Round(100.0 * $hits / ($hits + $misses), 2) } else { 0 }

    return @"
{"total_ops":$totalOps,"cache_hits":$hits,"cache_misses":$misses,"hit_rate":$hitRate,"current_keys":$keys,"write_through":$wtOps,"write_back":$wbOps,"segment_heat":$segHeatJson,"segment_requests":$segReqsJson}
"@
}

# ── Start HTTP listener ──────────────────────────────────────────
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://127.0.0.1:${HttpPort}/")
$listener.Start()

Write-Host ""
Write-Host "  Metrics HTTP server running on http://127.0.0.1:${HttpPort}" -ForegroundColor Cyan
Write-Host "  Dashboard:  http://127.0.0.1:${HttpPort}/dashboard.html" -ForegroundColor Green
Write-Host "  Metrics:    http://127.0.0.1:${HttpPort}/metrics" -ForegroundColor Green
Write-Host "  Press Ctrl+C to stop." -ForegroundColor Yellow
Write-Host ""

$webRoot = Join-Path $PSScriptRoot "..\web"

while ($listener.IsListening) {
    try {
        $ctx = $listener.GetContext()
    } catch {
        break
    }
    $req = $ctx.Request
    $res = $ctx.Response

    # CORS
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
        { $_ -eq "/" -or $_ -eq "/dashboard.html" } {
            $file = Join-Path $webRoot "dashboard.html"
            if (Test-Path $file) {
                $html = [System.IO.File]::ReadAllBytes($file)
                $res.ContentType = "text/html; charset=utf-8"
                $res.ContentLength64 = $html.Length
                $res.OutputStream.Write($html, 0, $html.Length)
            } else {
                $res.StatusCode = 404
                $buf = [System.Text.Encoding]::UTF8.GetBytes("dashboard.html not found at $file")
                $res.ContentLength64 = $buf.Length
                $res.OutputStream.Write($buf, 0, $buf.Length)
            }
        }
        default {
            # try serving static file from web/
            $file = Join-Path $webRoot ($path.TrimStart('/'))
            if (Test-Path $file) {
                $bytes = [System.IO.File]::ReadAllBytes($file)
                $ext = [System.IO.Path]::GetExtension($file)
                $ct = switch ($ext) {
                    ".html" { "text/html" }
                    ".css"  { "text/css" }
                    ".js"   { "application/javascript" }
                    ".json" { "application/json" }
                    ".png"  { "image/png" }
                    ".svg"  { "image/svg+xml" }
                    default { "application/octet-stream" }
                }
                $res.ContentType = $ct
                $res.ContentLength64 = $bytes.Length
                $res.OutputStream.Write($bytes, 0, $bytes.Length)
            } else {
                $res.StatusCode = 404
            }
        }
    }

    $res.Close()
}

$listener.Stop()

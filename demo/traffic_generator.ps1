# Traffic generator — sends a steady stream of SET/GET/DEL to the cache server
# so the dashboard has real data to visualize.
#
# Usage:  powershell -ExecutionPolicy Bypass -File demo\traffic_generator.ps1

param(
    [int]$Port = 6399,
    [int]$RequestsPerSecond = 40,   # roughly: 40 ops/s baseline
    [switch]$HotKeyBurst            # add a hot-key storm in the middle
)

$delay = [int](1000 / $RequestsPerSecond)

function Fire {
    param([string]$Cmd)
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect("127.0.0.1", $Port)
        $s = $c.GetStream()
        $b = [System.Text.Encoding]::ASCII.GetBytes("$Cmd`r`n")
        $s.Write($b, 0, $b.Length)
        $s.Flush()
        Start-Sleep -Milliseconds 20
        $buf = New-Object byte[] 512
        $null = $s.Read($buf, 0, $buf.Length)
        $c.Close()
    } catch {}
}

Write-Host "  Traffic generator started ($RequestsPerSecond ops/s target)" -ForegroundColor Cyan
Write-Host "  Press Ctrl+C to stop." -ForegroundColor Yellow
Write-Host ""

$iteration = 0
$userMax   = 500

while ($true) {
    $iteration++

    # ── Balanced traffic: SET and GET spread across many keys ──
    $uid = Get-Random -Minimum 1 -Maximum $userMax
    Fire "SET user:$uid data_$uid"

    $uid2 = Get-Random -Minimum 1 -Maximum $userMax
    Fire "GET user:$uid2"

    # occasional miss
    if ($iteration % 5 -eq 0) {
        Fire "GET nonexistent:$(Get-Random -Max 9999)"
    }

    # occasional DEL
    if ($iteration % 7 -eq 0) {
        Fire "DEL user:$(Get-Random -Minimum 1 -Maximum $userMax)"
    }

    # session keys
    $sid = Get-Random -Minimum 1 -Maximum 200
    Fire "SET session:$sid token_$(Get-Random -Max 99999)"
    Fire "GET session:$(Get-Random -Minimum 1 -Maximum 200)"

    # ── Hot-key burst (every 30 seconds for 5 seconds) ──
    if ($HotKeyBurst -and ($iteration % 120) -ge 100) {
        for ($b = 0; $b -lt 10; $b++) {
            Fire "GET user:42"    # single hot key
        }
    }

    # progress dot
    if ($iteration % 20 -eq 0) {
        Write-Host "." -NoNewline -ForegroundColor DarkGray
    }
    if ($iteration % 400 -eq 0) {
        Write-Host " [$iteration ops sent]" -ForegroundColor DarkGray
    }

    Start-Sleep -Milliseconds $delay
}

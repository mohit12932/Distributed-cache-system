# Visual demo of live server commands
# Shows each command sent and response received over TCP

$PORT = 6399

function Send-Redis {
    param([string]$Command, [string]$Label)
    
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect("127.0.0.1", $PORT)
    $stream = $client.GetStream()
    
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command + "`r`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    Start-Sleep -Milliseconds 150
    
    $buf = New-Object byte[] 8192
    $n = $stream.Read($buf, 0, $buf.Length)
    $response = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
    $client.Close()
    
    $display = $response.Replace("`r`n", " ").Trim()
    
    # Parse RESP
    if ($response.StartsWith("+")) {
        $display = ($response -split "`r`n")[0].Substring(1)
    } elseif ($response.StartsWith(":")) {
        $display = "(integer) " + ($response -split "`r`n")[0].Substring(1)
    } elseif ($response.StartsWith("`$-1")) {
        $display = "(nil)"
    } elseif ($response.StartsWith("`$")) {
        $parts = $response -split "`r`n"
        if ($parts.Count -ge 2) {
            $display = '"' + $parts[1] + '"'
        }
    } elseif ($response.StartsWith("*")) {
        $parts = $response -split "`r`n"
        $count = [int]$parts[0].Substring(1)
        $items = @()
        $idx = 1
        for ($i = 0; $i -lt $count; $i++) {
            if ($idx -lt $parts.Count -and $parts[$idx].StartsWith("`$")) {
                $idx++
                if ($idx -lt $parts.Count) {
                    $items += $parts[$idx]
                    $idx++
                }
            }
        }
        if ($items.Count -gt 0) {
            $display = ""
            for ($i = 0; $i -lt $items.Count; $i++) {
                $display += "  " + ($i + 1).ToString() + ") " + '"' + $items[$i] + '"' + "`n"
            }
            $display = $display.TrimEnd("`n")
        } else {
            $display = "(empty list)"
        }
    } elseif ($response.StartsWith("-ERR")) {
        $display = "(error) " + ($response -split "`r`n")[0].Substring(5)
    }
    
    Write-Host "  > " -NoNewline -ForegroundColor DarkGray
    Write-Host $Command -NoNewline -ForegroundColor White
    if ($Label) {
        Write-Host "   # $Label" -NoNewline -ForegroundColor DarkGray
    }
    Write-Host ""
    foreach ($dline in ($display -split "`n")) {
        Write-Host "    $dline" -ForegroundColor Cyan
    }
    Write-Host ""
}

Write-Host ""
Write-Host "  --- PING: Health check ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "PING" "Server alive?"
Send-Redis "PING hello" "Echo test"

Write-Host "  --- SET: Store key-value pairs ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "SET user:1001 Alice" "Store user"
Send-Redis "SET user:1002 Bob" "Store another user"
Send-Redis "SET session:abc token_xyz" "Store session"
Send-Redis "SET counter 42" "Store number"

Write-Host "  --- GET: Retrieve values ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "GET user:1001" "Found!"
Send-Redis "GET user:1002" "Found!"
Send-Redis "GET nonexistent" "Cache miss"

Write-Host "  --- EXISTS: Check key presence ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "EXISTS user:1001" "Key exists = 1"
Send-Redis "EXISTS ghost" "Key missing = 0"

Write-Host "  --- UPDATE: Overwrite existing key ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "SET user:1001 Alice_Updated" "Overwrite"
Send-Redis "GET user:1001" "Returns new value"

Write-Host "  --- DEL: Delete keys ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "DEL session:abc" "Delete session"
Send-Redis "GET session:abc" "Verify deleted"

Write-Host "  --- DBSIZE: Total keys in cache ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "DBSIZE" "Count all keys"

Write-Host "  --- KEYS: List all cached keys ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "KEYS *" "List everything"

Write-Host "  --- INFO: Server statistics ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "INFO" "Full stats"

Write-Host "  --- PERSISTENCE: Check disk file ---" -ForegroundColor Yellow
Write-Host ""
Write-Host "  File: data/demo.dat" -ForegroundColor Gray
Write-Host "  -------------------------------------------" -ForegroundColor DarkGray
$fileContent = Get-Content "data/demo.dat" -ErrorAction SilentlyContinue
if ($fileContent) {
    foreach ($line in $fileContent) {
        $parts = $line -split "`t"
        $k = $parts[0]
        $v = ""
        if ($parts.Count -gt 1) { $v = $parts[1] }
        Write-Host "    $k  =  $v" -ForegroundColor Cyan
    }
}
Write-Host "  -------------------------------------------" -ForegroundColor DarkGray
Write-Host "  Data is persisted to disk (Write-Through)" -ForegroundColor Green
Write-Host ""

Write-Host "  --- FLUSHALL: Clear entire cache ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "FLUSHALL" "Wipe everything"
Send-Redis "DBSIZE" "Verify empty"

Write-Host "  --- ERROR HANDLING ---" -ForegroundColor Yellow
Write-Host ""
Send-Redis "BADCOMMAND" "Unknown command"
Send-Redis "GET" "Missing argument"

Write-Host ""
Write-Host "  =========================================================" -ForegroundColor Green
Write-Host "  All live server operations demonstrated successfully!" -ForegroundColor Green
Write-Host "  =========================================================" -ForegroundColor Green
Write-Host ""

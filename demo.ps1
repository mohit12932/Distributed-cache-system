Write-Host "Starting Distributed Cache Cluster..." -ForegroundColor Cyan
docker-compose up -d

Write-Host "Waiting for Cluster to initialize..." -ForegroundColor Yellow
Start-Sleep -Seconds 10

Write-Host "Opening Live Grafana Dashboard..." -ForegroundColor Green
Start-Process "http://localhost:3000/d/dcs1/distributed-cache-system?orgId=1&refresh=5s"

Write-Host "Killing any existing zombie load tests..." -ForegroundColor Yellow
docker rm -f redis_stress_test 2>$null
docker rm -f lsm_stress_test 2>$null

Write-Host "Generating Python Hotspot Script..." -ForegroundColor DarkGray
$pythonScript = @"
import socket, time, threading, random
def attack_hotspot():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', 6379))
        s.sendall(b'*2\r\n$4\r\nAUTH\r\n$13\r\nMySuperSecret\r\n')
        s.recv(1024)
        while True:
            key = 'THUNDERING_HERD_KEY' if random.random() < 0.99 else f'random_{random.randint(1,100)}'
            cmd = f'*3\r\n$3\r\nSET\r\n${len(key)}\r\n{key}\r\n$5\r\nVALUE\r\n'
            s.sendall(cmd.encode())
            s.recv(1024)
    except: pass
for _ in range(50):
    threading.Thread(target=attack_hotspot, daemon=True).start()
while True: time.sleep(1)
"@
Set-Content -Path "trigger_hotspot.py" -Value $pythonScript

Write-Host "Launching 1. Uniform RPS Stress Test..." -ForegroundColor Red
Start-Process -NoNewWindow -FilePath "docker" -ArgumentList "run --rm --name redis_stress_test --network distributedcachesystem_default redis redis-benchmark -h haproxy -p 6379 -a MySuperSecret -c 500 -n 2000000 --threads 4 -P 200 -t set,get -l"

Write-Host "Launching 2. LSM-Tree Compaction Test (10M Unique Keys)..." -ForegroundColor Cyan
Start-Process -NoNewWindow -FilePath "docker" -ArgumentList "run --rm --name lsm_stress_test --network distributedcachesystem_default redis redis-benchmark -h haproxy -p 6379 -a MySuperSecret -c 500 -n 5000000 -r 10000000 --threads 4 -P 200 -t set -l"

Write-Host "Launching 3. ML Hotspot Migration Attack (Thundering Herd)..." -ForegroundColor Magenta
Start-Process -NoNewWindow -FilePath "python" -ArgumentList "trigger_hotspot.py"

Write-Host "`nAll 3 Attacks are running simultaneously in the background!" -ForegroundColor Green
Write-Host "Check your Grafana dashboard to see ALL metrics light up at once!" -ForegroundColor White
Write-Host "(Press Ctrl+C to cleanly stop all attacks)" -ForegroundColor DarkGray

try {
    while ($true) {
        Start-Sleep -Seconds 1
    }
} finally {
    Write-Host "`nCatching Ctrl+C! Cleaning up all stress tests..." -ForegroundColor Yellow
    docker rm -f redis_stress_test 2>$null
    docker rm -f lsm_stress_test 2>$null
    Stop-Process -Name "python" -ErrorAction SilentlyContinue
    
    Write-Host "Demo Complete! The cluster is still running in the background." -ForegroundColor Green
    Write-Host "To shut down the cluster completely, run: docker-compose down" -ForegroundColor Gray
}

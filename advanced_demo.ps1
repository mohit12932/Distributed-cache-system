Write-Host "Starting Distributed Cache Cluster..." -ForegroundColor Cyan
docker-compose up -d

Write-Host "Waiting for Cluster to initialize..." -ForegroundColor Yellow
Start-Sleep -Seconds 5

Write-Host "Killing any existing simple load tests..." -ForegroundColor Yellow
docker rm -f redis_stress_test 2>$null
docker rm -f lsm_stress_test 2>$null

Write-Host "Triggering LSM-Tree Compactions (10 Million Unique Keys)..." -ForegroundColor Cyan
# The -r 10000000 flag forces the benchmark to write 10,000,000 unique keys.
# We use only "set" commands to rapidly fill the Memtable and force disk flushes.
Start-Process -NoNewWindow -FilePath "docker" -ArgumentList "run --rm --name lsm_stress_test --network distributedcachesystem_default redis redis-benchmark -h haproxy -p 6379 -a MySuperSecret -c 500 -n 5000000 -r 10000000 --threads 4 -P 200 -t set -l"

Write-Host "Triggering ML PINN Hotspot Migrations (99% Skewed Traffic)..." -ForegroundColor Magenta
# We use Python to aggressively spam a single key, creating a massive artificial anomaly.
$pythonScript = @"
import socket, time, threading, random

def attack_hotspot():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', 6379))
        s.sendall(b'*2\r\n$4\r\nAUTH\r\n$13\r\nMySuperSecret\r\n')
        s.recv(1024)
        
        while True:
            # 99% of traffic hits a single key to trigger the ML anomaly detection
            key = 'THUNDERING_HERD_KEY' if random.random() < 0.99 else f'random_{random.randint(1,100)}'
            cmd = f'*3\r\n$3\r\nSET\r\n${len(key)}\r\n{key}\r\n$5\r\nVALUE\r\n'
            s.sendall(cmd.encode())
            s.recv(1024)
    except:
        pass

for _ in range(50):
    threading.Thread(target=attack_hotspot, daemon=True).start()

print('Hotspot Attack Running... (Check Grafana!)')
print('Press Ctrl+C to stop the attack.')
while True:
    time.sleep(1)
"@

Set-Content -Path "trigger_hotspot.py" -Value $pythonScript
python trigger_hotspot.py

Write-Host "Cleaning up tests..." -ForegroundColor Yellow
docker rm -f lsm_stress_test 2>$null

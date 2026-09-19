Write-Host "Starting Distributed Cache Cluster..." -ForegroundColor Cyan
docker-compose up -d

Write-Host "Waiting for Grafana to initialize..." -ForegroundColor Yellow
Start-Sleep -Seconds 10

Write-Host "Opening Live Grafana Dashboard..." -ForegroundColor Green
Start-Process "http://localhost:3000/d/dcs1/distributed-cache-system?orgId=1&refresh=5s"

Write-Host "Launching 1.8M+ RPS Stress Test via HAProxy..." -ForegroundColor Red
Write-Host "(Press Ctrl+C at any time to STOP the load test)" -ForegroundColor DarkGray

try {
    # -it allows Ctrl+C to propagate to the container, --name lets us clean it up reliably
    docker run -it --rm --name redis_stress_test --network distributedcachesystem_default redis redis-benchmark -h haproxy -p 6379 -a MySuperSecret -c 1000 -n 2000000 --threads 8 -P 500 -t set,get -l
} finally {
    Write-Host "`nStopping stress test..." -ForegroundColor Yellow
    docker rm -f redis_stress_test 2>$null
    Write-Host "Demo Complete! The cluster is still running in the background." -ForegroundColor Green
    Write-Host "To shut down the cluster completely, run: docker-compose down" -ForegroundColor Gray
}

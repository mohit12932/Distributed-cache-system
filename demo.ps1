Write-Host "Starting Distributed Cache Cluster..." -ForegroundColor Cyan
docker-compose up -d

Write-Host "Waiting for Grafana to initialize..." -ForegroundColor Yellow
Start-Sleep -Seconds 10

Write-Host "Opening Live Grafana Dashboard..." -ForegroundColor Green
Start-Process "http://localhost:3000/d/dcs1/distributed-cache-system?orgId=1&refresh=5s"

Write-Host "Launching 1.8M+ RPS Stress Test via HAProxy..." -ForegroundColor Red
docker run --rm --network distributedcachesystem_default redis redis-benchmark -h haproxy -p 6379 -a MySuperSecret -c 1000 -n 2000000 --threads 8 -P 500 -t set,get -l

Write-Host "Demo Complete!" -ForegroundColor Green

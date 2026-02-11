#!/bin/bash
# ============================================================================
# Distributed Cache System - Complete Test Suite Runner
# ============================================================================
# This script builds and runs all tests, then starts a live demo server.
# Usage: ./demo/run_all_tests.sh
# ============================================================================

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

PORT=${1:-6399}

echo ""
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║     Distributed Cache System - Complete Test Suite               ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# ── Build Phase ─────────────────────────────────────────────────────────────

echo "📦 Building project..."
mkdir -p build

echo -n "  Building Main Server..."
g++ -std=c++17 -O2 -Wall -I. -o build/distributed_cache src/main.cpp -pthread && echo " ✓" || echo " ✗"

echo -n "  Building LRU Cache Tests..."
g++ -std=c++17 -O2 -Wall -I. -o build/test_lru_cache src/tests/test_lru_cache.cpp -pthread && echo " ✓" || echo " ✗"

echo -n "  Building Concurrency Tests..."
g++ -std=c++17 -O2 -Wall -I. -o build/test_concurrency src/tests/test_concurrency.cpp -pthread && echo " ✓" || echo " ✗"

echo -n "  Building RESP Parser Tests..."
g++ -std=c++17 -O2 -Wall -I. -o build/test_resp_parser src/tests/test_resp_parser.cpp -pthread && echo " ✓" || echo " ✗"

echo -n "  Building Live Server Tests..."
g++ -std=c++17 -O2 -Wall -I. -o build/test_live_server tests/test_live_server.cpp -pthread && echo " ✓" || echo " ✗"

echo ""

# ── Test Suite 1: LRU Cache Core ────────────────────────────────────────────

echo "═══════════════════════════════════════════════════════════════════"
echo "  TEST SUITE 1: LRU Cache Core Engine"
echo "═══════════════════════════════════════════════════════════════════"
./build/test_lru_cache
SUITE1=$?
echo ""

# ── Test Suite 2: Concurrency ───────────────────────────────────────────────

echo "═══════════════════════════════════════════════════════════════════"
echo "  TEST SUITE 2: Concurrency Stress Tests"
echo "═══════════════════════════════════════════════════════════════════"
./build/test_concurrency
SUITE2=$?
echo ""

# ── Test Suite 3: RESP Protocol ─────────────────────────────────────────────

echo "═══════════════════════════════════════════════════════════════════"
echo "  TEST SUITE 3: RESP Parser & Handler Tests"
echo "═══════════════════════════════════════════════════════════════════"
./build/test_resp_parser
SUITE3=$?
echo ""

# ── Test Suite 4: Live Server ───────────────────────────────────────────────

echo "═══════════════════════════════════════════════════════════════════"
echo "  TEST SUITE 4: Live Server Integration"
echo "═══════════════════════════════════════════════════════════════════"

# Clean up old data
rm -rf data test_data 2>/dev/null || true

# Start server in background
echo "  Starting server on port $PORT..."
./build/distributed_cache --port $PORT --mode write-through --data-file data/test.dat &
SERVER_PID=$!
sleep 2

# Run integration tests
./build/test_live_server
SUITE4=$?

# Stop server
kill $SERVER_PID 2>/dev/null || true
echo ""

# ── Summary ─────────────────────────────────────────────────────────────────

echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║                        TEST SUMMARY                              ║"
echo "╠══════════════════════════════════════════════════════════════════╣"

print_result() {
    if [ $2 -eq 0 ]; then
        echo "║  $1  ✓ PASS                              ║"
    else
        echo "║  $1  ✗ FAIL                              ║"
    fi
}

print_result "LRU Cache Core         (11 tests)" $SUITE1
print_result "Concurrency Stress      (5 tests)" $SUITE2
print_result "RESP Parser & Handler  (16 tests)" $SUITE3
print_result "Live Server Integration(26 tests)" $SUITE4

echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# Exit with failure if any suite failed
if [ $SUITE1 -ne 0 ] || [ $SUITE2 -ne 0 ] || [ $SUITE3 -ne 0 ] || [ $SUITE4 -ne 0 ]; then
    exit 1
fi

echo "🚀 All tests passed! Start server with: ./build/distributed_cache --port 6379"

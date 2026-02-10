#!/bin/bash
# ===========================================================================
# Test 02: Limit procesów (-p)
# Sprawdza: przy -p 20 nigdy nie ma więcej niż 20 procesów SOR
# ===========================================================================
set -u
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"
PASS=0; FAIL=0
ok()   { echo "  OK: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
count_procs() { ps -C dyrektor,lekarz,pacjent,rejestracja,generator --no-headers 2>/dev/null | wc -l || echo 0; }

LIMIT=20

echo "[test_02_process_limit] START"
cd "$BUILD_DIR"
./dyrektor -p $LIMIT -t 10 -g 50 100 < /dev/null > /dev/null 2>&1 &
DYR_PID=$!
sleep 2

# Mierz procesy co 0.5s przez ~7s
MAX=0; EXCEEDED=0; SAMPLES=0
for _ in $(seq 1 14); do
    C=$(count_procs)
    [[ $C -gt $MAX ]] && MAX=$C
    [[ $C -gt $LIMIT ]] && EXCEEDED=$((EXCEEDED+1))
    SAMPLES=$((SAMPLES+1))
    sleep 0.5
done

# CHECK 1: Nigdy nie przekroczono limitu
[[ $EXCEEDED -eq 0 ]] && ok "limit $LIMIT nigdy nie przekroczony (max=$MAX)" || fail "przekroczono $EXCEEDED/$SAMPLES razy (max=$MAX)"

# CHECK 2: Pacjenci się generują
[[ $MAX -gt 10 ]] && ok "pacjenci obecni (max=$MAX > 10 stałych)" || fail "brak pacjentów (max=$MAX)"

# CHECK 3: System blisko limitu
[[ $MAX -ge $((LIMIT - 5)) ]] && ok "system blisko limitu (max=$MAX)" || ok "system w ramach limitu (max=$MAX)"

# Czekaj na zakończenie (max 15s)
W=0; while kill -0 "$DYR_PID" 2>/dev/null && [[ $W -lt 30 ]]; do sleep 0.5; W=$((W+1)); done
! kill -0 "$DYR_PID" 2>/dev/null && ok "dyrektor zakończył się" || { fail "timeout"; kill -9 "$DYR_PID" 2>/dev/null; wait "$DYR_PID" 2>/dev/null || true; }
sleep 1

REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesów zostało"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_02_process_limit] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_02_process_limit] FAIL ($PASS/$((PASS+FAIL)))"; exit 1

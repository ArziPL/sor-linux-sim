#!/bin/bash
# ===========================================================================
# Test 03: Stress-test — brak zakleszczenia przy -g 10 20
# Sprawdza: program działa 12s pod obciążeniem i kończy się (brak zakleszczenia)
# ===========================================================================
set -u
BUILD_DIR="$(cd "$(dirname "$0")/.." && pwd)/build"
PASS=0; FAIL=0
ok()   { echo "  OK: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
count_procs() { ps -C dyrektor,lekarz,pacjent,rejestracja,generator --no-headers 2>/dev/null | wc -l || echo 0; }
our_shm() { ipcs -m 2>/dev/null | grep "^0x" | grep -v "^0x00000000" | wc -l || echo 0; }
our_sem() { ipcs -s 2>/dev/null | grep "^0x" | wc -l || echo 0; }
our_msg() { ipcs -q 2>/dev/null | grep "^0x" | wc -l || echo 0; }

echo "[test_03_stress_no_deadlock] START"
cd "$BUILD_DIR"
./dyrektor -g 10 20 -t 12 < /dev/null > /dev/null 2>&1 &
DYR_PID=$!
sleep 2

# Monitoruj ~10s
MAX_PROCS=0
for _ in $(seq 1 10); do
    C=$(count_procs)
    [[ $C -gt $MAX_PROCS ]] && MAX_PROCS=$C
    sleep 1
done

# CHECK 1: Pacjenci generowani
[[ $MAX_PROCS -gt 10 ]] && ok "pacjenci pod obciążeniem (max=$MAX_PROCS)" || fail "brak pacjentów (max=$MAX_PROCS)"

# CHECK 2: Brak zakleszczenia — program kończy się (max 20s)
W=0; while kill -0 "$DYR_PID" 2>/dev/null && [[ $W -lt 40 ]]; do sleep 0.5; W=$((W+1)); done
if ! kill -0 "$DYR_PID" 2>/dev/null; then
    ok "brak zakleszczenia — program zakończył się"
else
    fail "ZAKLESZCZENIE — program nie zakończył się w 20s"
    kill -9 "$DYR_PID" 2>/dev/null; wait "$DYR_PID" 2>/dev/null || true
    pkill -9 -x lekarz 2>/dev/null || true; pkill -9 -x pacjent 2>/dev/null || true
    pkill -9 -x rejestracja 2>/dev/null || true; pkill -9 -x generator 2>/dev/null || true
fi
sleep 2

# CHECK 3: Sprzątanie
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesów zostało"

# CHECK 4: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_03_stress_no_deadlock] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_03_stress_no_deadlock] FAIL ($PASS/$((PASS+FAIL)))"; exit 1

#!/bin/bash
# ===========================================================================
# Test 01: Normalne uruchomienie i sprzątanie
# Sprawdza: procesy startują, IPC istnieje, po -t kończy się, czyści IPC+procesy
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

echo "[test_01_startup_cleanup] START"
cd "$BUILD_DIR"
./dyrektor -t 5 < /dev/null > /dev/null 2>&1 &
DYR_PID=$!
sleep 2

# Procesy żyją
CNT=$(count_procs)
[[ $CNT -ge 10 ]] && ok "$CNT procesów SOR (>= 10)" || fail "tylko $CNT procesów"
kill -0 "$DYR_PID" 2>/dev/null && ok "dyrektor żyje" || fail "dyrektor nie żyje"

# IPC istnieje
[[ $(our_shm) -ge 1 ]] && ok "SHM istnieje" || fail "brak SHM"
[[ $(our_sem) -ge 1 ]] && ok "SEM istnieje" || fail "brak SEM"
[[ $(our_msg) -ge 1 ]] && ok "MSG istnieje" || fail "brak MSG"

# Czekaj na zakończenie (max 12s)
W=0; while kill -0 "$DYR_PID" 2>/dev/null && [[ $W -lt 24 ]]; do sleep 0.5; W=$((W+1)); done
! kill -0 "$DYR_PID" 2>/dev/null && ok "dyrektor zakończył się" || { fail "timeout"; kill -9 "$DYR_PID" 2>/dev/null; wait "$DYR_PID" 2>/dev/null || true; }
sleep 2

# Po zakończeniu: brak procesów
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "brak procesów SOR" || fail "$REM procesów zostało"

# Po zakończeniu: IPC czyste
[[ $(our_shm) -eq 0 ]] && ok "SHM czyste" || fail "SHM zostało"
[[ $(our_sem) -eq 0 ]] && ok "SEM czyste" || fail "SEM zostało"
[[ $(our_msg) -eq 0 ]] && ok "MSG czyste" || fail "MSG zostało"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_01_startup_cleanup] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_01_startup_cleanup] FAIL ($PASS/$((PASS+FAIL)))"; exit 1

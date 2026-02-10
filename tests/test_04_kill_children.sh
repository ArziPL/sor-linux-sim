#!/bin/bash
# ===========================================================================
# Test 04: Zabicie procesów potomnych — dyrektor sprząta poprawnie
# Sprawdza: po SIGKILL 2 lekarzy + rejestracji, dyrektor nadal żyje i sprząta
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

echo "[test_04_kill_children] START"
cd "$BUILD_DIR"
./dyrektor -t 12 -g 200 400 < /dev/null > /dev/null 2>&1 &
DYR_PID=$!
sleep 3

# Zbierz PIDy i zabij 2 lekarzy + rejestrację (NIE generator — jego śmierć zabija dyrektora przez prctl)
KILLED=0
for PID in $(pgrep -x lekarz 2>/dev/null | head -2); do
    kill -9 "$PID" 2>/dev/null && KILLED=$((KILLED+1)) && echo "  INFO: zabito lekarz PID=$PID"
done
REJ_PID=$(pgrep -x rejestracja 2>/dev/null | head -1 || true)
if [[ -n "$REJ_PID" ]]; then
    kill -9 "$REJ_PID" 2>/dev/null && KILLED=$((KILLED+1)) && echo "  INFO: zabito rejestracja PID=$REJ_PID"
fi

# CHECK 1: Zabiliśmy >= 2
[[ $KILLED -ge 2 ]] && ok "zabito $KILLED procesów" || fail "zabito tylko $KILLED"

sleep 1

# CHECK 2: Dyrektor przeżył
kill -0 "$DYR_PID" 2>/dev/null && ok "dyrektor przeżył" || fail "dyrektor umarł"

# Czekaj na zakończenie (max 18s)
W=0; while kill -0 "$DYR_PID" 2>/dev/null && [[ $W -lt 36 ]]; do sleep 0.5; W=$((W+1)); done
! kill -0 "$DYR_PID" 2>/dev/null && ok "dyrektor zakończył się" || { fail "timeout"; kill -9 "$DYR_PID" 2>/dev/null; wait "$DYR_PID" 2>/dev/null || true; }
sleep 2

# CHECK 3: Brak procesów
REM=$(count_procs)
[[ $REM -eq 0 ]] && ok "procesy wyczyszczone" || fail "$REM procesów zostało"

# CHECK 4: IPC czyste
SHM=$(our_shm); SEM=$(our_sem); MSG=$(our_msg)
[[ $SHM -eq 0 && $SEM -eq 0 && $MSG -eq 0 ]] && ok "IPC czyste" || fail "IPC: shm=$SHM sem=$SEM msg=$MSG"

echo ""; [[ $FAIL -eq 0 ]] && echo "[test_04_kill_children] PASS ($PASS/$((PASS+FAIL)))" && exit 0
echo "[test_04_kill_children] FAIL ($PASS/$((PASS+FAIL)))"; exit 1

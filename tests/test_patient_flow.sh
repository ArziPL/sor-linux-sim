#!/bin/bash
# test_patient_flow.sh
# Test: Pacjenci przechodzą pełną ścieżkę, zajętość ≤ 20, brak zombies
#
# Uruchamia symulację na 15 sekund i sprawdza log:
#   1. Przynajmniej 1 pacjent ukończył leczenie lub został odesłany
#   2. Zajętość SOR nigdy nie przekroczyła N=20
#   3. Brak procesów-zombie po zakończeniu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
LOG_FILE="$SCRIPT_DIR/../sor_log.txt"
TEST_NAME="test_patient_flow"

echo "[$TEST_NAME] START"

cd "$BUILD_DIR" || { echo "[$TEST_NAME] FAIL: brak katalogu build"; exit 1; }

# Zapamiętaj IPC sprzed testu
IPC_BEFORE_SHM=$(ipcs -m | grep -c "^0x")
IPC_BEFORE_SEM=$(ipcs -s | grep -c "^0x")
IPC_BEFORE_MSG=$(ipcs -q | grep -c "^0x")

# Uruchom symulację na 15s
exec 3< <(sleep 15; echo "q")
./dyrektor <&3 >/dev/null 2>&1 &
DYREKTOR_PID=$!

# Czekaj na zakończenie
WAIT_COUNT=0
while kill -0 "$DYREKTOR_PID" 2>/dev/null && [[ $WAIT_COUNT -lt 50 ]]; do
    sleep 0.5
    WAIT_COUNT=$((WAIT_COUNT + 1))
done

if kill -0 "$DYREKTOR_PID" 2>/dev/null; then
    kill -9 "$DYREKTOR_PID" 2>/dev/null
    wait "$DYREKTOR_PID" 2>/dev/null
fi
wait "$DYREKTOR_PID" 2>/dev/null
exec 3<&-
sleep 1

PASS=1

# --- CHECK 1: Log istnieje i ma treść ---
if [[ ! -f "$LOG_FILE" ]]; then
    echo "  FAIL: brak pliku logu $LOG_FILE"
    echo "[$TEST_NAME] FAIL"
    exit 1
fi

LINE_COUNT=$(wc -l < "$LOG_FILE")
if [[ "$LINE_COUNT" -lt 10 ]]; then
    echo "  FAIL: log ma tylko $LINE_COUNT linii — symulacja mogła się nie uruchomić"
    PASS=0
fi

# --- CHECK 2: Przynajmniej 1 pacjent ukończył leczenie lub został odesłany ---
COMPLETED=$(grep -cE "(opuszcza SOR|odesłan|wychodzi z SOR)" "$LOG_FILE" || true)
if [[ "$COMPLETED" -lt 1 ]]; then
    echo "  FAIL: żaden pacjent nie ukończył ścieżki (completed=$COMPLETED)"
    PASS=0
else
    echo "  OK: $COMPLETED pacjentów ukończyło ścieżkę"
fi

# --- CHECK 3: Zajętość SOR nigdy nie > 20 ---
MAX_OCCUPANCY=$(grep -oP 'w poczekalni: \K[0-9]+' "$LOG_FILE" 2>/dev/null | sort -rn | head -1)
if [[ -z "$MAX_OCCUPANCY" ]]; then
    # Alternatywny pattern — szukaj "pacjentów w SOR" lub "patients_in_sor"
    MAX_OCCUPANCY=$(grep -oP 'w SOR: \K[0-9]+' "$LOG_FILE" 2>/dev/null | sort -rn | head -1)
fi

if [[ -n "$MAX_OCCUPANCY" && "$MAX_OCCUPANCY" -gt 20 ]]; then
    echo "  FAIL: zajętość SOR=$MAX_OCCUPANCY > 20"
    PASS=0
elif [[ -n "$MAX_OCCUPANCY" ]]; then
    echo "  OK: max zajętość SOR=$MAX_OCCUPANCY (limit 20)"
fi

# --- CHECK 4: Brak zombie procesów ---
ZOMBIES=$(ps -eo stat,cmd | grep -E "^Z.*(lekarz|pacjent|rejestracja)" | wc -l)
if [[ "$ZOMBIES" -gt 0 ]]; then
    echo "  FAIL: $ZOMBIES zombie procesów nadal istnieje"
    PASS=0
fi

# --- CHECK 5: IPC sprzątnięte ---
IPC_AFTER_SHM=$(ipcs -m | grep -c "^0x")
IPC_AFTER_SEM=$(ipcs -s | grep -c "^0x")
IPC_AFTER_MSG=$(ipcs -q | grep -c "^0x")

if [[ "$IPC_AFTER_SHM" -gt "$IPC_BEFORE_SHM" ]]; then
    echo "  FAIL: wyciek shared memory"
    PASS=0
fi
if [[ "$IPC_AFTER_SEM" -gt "$IPC_BEFORE_SEM" ]]; then
    echo "  FAIL: wyciek semaforów"
    PASS=0
fi
if [[ "$IPC_AFTER_MSG" -gt "$IPC_BEFORE_MSG" ]]; then
    echo "  FAIL: wyciek kolejek komunikatów"
    PASS=0
fi

if [[ "$PASS" -eq 1 ]]; then
    echo "[$TEST_NAME] PASS"
    exit 0
else
    echo "[$TEST_NAME] FAIL"
    exit 1
fi

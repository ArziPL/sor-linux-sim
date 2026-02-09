#!/bin/bash
# test_startup_cleanup.sh
# Test: Symulacja uruchamia wszystkie procesy i czyści IPC po zamknięciu
#
# Sprawdza:
#   1. Po uruchomieniu dyrektora istnieją procesy: rejestracja, lekarz (7×)
#   2. IPC istnieje: shared memory, semafory, kolejki komunikatów
#   3. Po zamknięciu (q) wszystkie zasoby IPC są usunięte, brak zombies

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"
TEST_NAME="test_startup_cleanup"

echo "[$TEST_NAME] START"

cd "$BUILD_DIR" || { echo "[$TEST_NAME] FAIL: brak katalogu build"; exit 1; }

# Sprawdź czy binaria istnieją
for bin in dyrektor rejestracja lekarz pacjent; do
    if [[ ! -x "./$bin" ]]; then
        echo "[$TEST_NAME] FAIL: brak pliku wykonywalnego ./$bin"
        exit 1
    fi
done

# Zapamiętaj IPC przed testem
IPC_BEFORE_SHM=$(ipcs -m | grep -c "^0x")
IPC_BEFORE_SEM=$(ipcs -s | grep -c "^0x")
IPC_BEFORE_MSG=$(ipcs -q | grep -c "^0x")

# Uruchom dyrektora w tle (stdin z pipe, żebyśmy mogli wysłać 'q')
exec 3< <(sleep 3; echo "q")
./dyrektor <&3 >/dev/null 2>&1 &
DYREKTOR_PID=$!

# Daj chwilę na start procesów
sleep 1.5

PASS=1

# --- CHECK 1: Procesy potomne ---
REJ_COUNT=$(pgrep -P "$DYREKTOR_PID" -a 2>/dev/null | grep -c "rejestracja" || true)
LEK_COUNT=$(pgrep -P "$DYREKTOR_PID" -a 2>/dev/null | grep -c "lekarz" || true)

if [[ "$REJ_COUNT" -lt 1 ]]; then
    echo "  FAIL: brak procesu rejestracja (znaleziono: $REJ_COUNT)"
    PASS=0
fi
if [[ "$LEK_COUNT" -lt 7 ]]; then
    echo "  FAIL: oczekiwano 7 lekarzy, znaleziono: $LEK_COUNT"
    PASS=0
fi

# --- CHECK 2: IPC istnieje (więcej niż przed testem) ---
IPC_NOW_SHM=$(ipcs -m | grep -c "^0x")
IPC_NOW_SEM=$(ipcs -s | grep -c "^0x")
IPC_NOW_MSG=$(ipcs -q | grep -c "^0x")

if [[ "$IPC_NOW_SHM" -le "$IPC_BEFORE_SHM" ]]; then
    echo "  FAIL: brak nowego segmentu shared memory"
    PASS=0
fi
if [[ "$IPC_NOW_SEM" -le "$IPC_BEFORE_SEM" ]]; then
    echo "  FAIL: brak nowego zestawu semaforów"
    PASS=0
fi
if [[ "$IPC_NOW_MSG" -le "$IPC_BEFORE_MSG" ]]; then
    echo "  FAIL: brak nowych kolejek komunikatów"
    PASS=0
fi

# Czekaj na zakończenie dyrektora (max 10s)
WAIT_COUNT=0
while kill -0 "$DYREKTOR_PID" 2>/dev/null && [[ $WAIT_COUNT -lt 20 ]]; do
    sleep 0.5
    WAIT_COUNT=$((WAIT_COUNT + 1))
done

if kill -0 "$DYREKTOR_PID" 2>/dev/null; then
    echo "  WARN: dyrektor nie zakończył się w 10s, wymuszam zakończenie"
    kill -9 "$DYREKTOR_PID" 2>/dev/null
    wait "$DYREKTOR_PID" 2>/dev/null
fi
wait "$DYREKTOR_PID" 2>/dev/null
exec 3<&-

# Odczekaj na pełne sprzątanie
sleep 1

# --- CHECK 3: IPC po zamknięciu powinno wrócić do stanu sprzed testu ---
IPC_AFTER_SHM=$(ipcs -m | grep -c "^0x")
IPC_AFTER_SEM=$(ipcs -s | grep -c "^0x")
IPC_AFTER_MSG=$(ipcs -q | grep -c "^0x")

if [[ "$IPC_AFTER_SHM" -gt "$IPC_BEFORE_SHM" ]]; then
    echo "  FAIL: shared memory nie zostało sprzątnięte (przed=$IPC_BEFORE_SHM, po=$IPC_AFTER_SHM)"
    PASS=0
fi
if [[ "$IPC_AFTER_SEM" -gt "$IPC_BEFORE_SEM" ]]; then
    echo "  FAIL: semafory nie zostały sprzątnięte (przed=$IPC_BEFORE_SEM, po=$IPC_AFTER_SEM)"
    PASS=0
fi
if [[ "$IPC_AFTER_MSG" -gt "$IPC_BEFORE_MSG" ]]; then
    echo "  FAIL: kolejki komunikatów nie zostały sprzątnięte (przed=$IPC_BEFORE_MSG, po=$IPC_AFTER_MSG)"
    PASS=0
fi

# --- CHECK 4: Brak zombies ---
ZOMBIE_COUNT=$(ps -eo stat,ppid | awk -v pid="$DYREKTOR_PID" '$1 ~ /Z/ && $2 == pid' | wc -l)
if [[ "$ZOMBIE_COUNT" -gt 0 ]]; then
    echo "  FAIL: znaleziono $ZOMBIE_COUNT zombie procesów"
    PASS=0
fi

if [[ "$PASS" -eq 1 ]]; then
    echo "[$TEST_NAME] PASS"
    exit 0
else
    echo "[$TEST_NAME] FAIL"
    exit 1
fi

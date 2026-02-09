#!/bin/bash
# run_tests.sh — Uruchom wszystkie testy symulacji SOR
# Użycie: ./tests/run_tests.sh

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color
BOLD='\033[1m'

PASSED=0
FAILED=0
TOTAL=0
FAILED_NAMES=()

echo -e "${BOLD}${CYAN}============================================${NC}"
echo -e "${BOLD}${CYAN}  TESTY SYMULACJI SOR                       ${NC}"
echo -e "${BOLD}${CYAN}============================================${NC}"
echo ""

# --- KROK 1: Budowanie ---
echo -e "${YELLOW}[BUILD]${NC} Budowanie projektu i testów..."
cd "$BUILD_DIR" || { echo "FAIL: brak katalogu build"; exit 1; }
cmake .. -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
if [[ $? -ne 0 ]]; then
    echo -e "${RED}[BUILD] FAIL: kompilacja nie powiodła się${NC}"
    exit 1
fi
echo -e "${GREEN}[BUILD]${NC} OK"
echo ""

# --- KROK 2: Testy C++ (podłączają się do prawdziwej symulacji) ---
echo -e "${BOLD}--- Testy IPC na żywej symulacji (C++) ---${NC}"

run_cpp_test() {
    local name="$1"
    TOTAL=$((TOTAL + 1))
    
    local output
    output=$(cd "$BUILD_DIR" && ./"$name" 2>&1)
    local rc=$?
    
    if [[ $rc -eq 0 ]]; then
        echo -e "  ${GREEN}✓ PASS${NC}  $name"
        echo "$output" | grep "  OK:" | sed 's/^/    /'
        PASSED=$((PASSED + 1))
    else
        echo -e "  ${RED}✗ FAIL${NC}  $name"
        echo "$output" | grep -E "(FAIL|OK):" | sed 's/^/    /'
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
}

run_cpp_test "test_ipc_init"
run_cpp_test "test_occupancy_invariant"
run_cpp_test "test_doctor_signal"

echo ""

# --- KROK 3: Testy symulacji (bash) ---
echo -e "${BOLD}--- Testy symulacji (integracyjne) ---${NC}"

run_bash_test() {
    local script="$1"
    local name
    name=$(basename "$script" .sh)
    TOTAL=$((TOTAL + 1))
    
    local output
    output=$(bash "$script" 2>&1)
    local rc=$?
    
    if [[ $rc -eq 0 ]]; then
        echo -e "  ${GREEN}✓ PASS${NC}  $name"
        # Pokaż linie OK 
        echo "$output" | grep "  OK:" | sed 's/^/    /'
        PASSED=$((PASSED + 1))
    else
        echo -e "  ${RED}✗ FAIL${NC}  $name"
        echo "$output" | grep -E "(FAIL|OK):" | sed 's/^/    /'
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
}

run_bash_test "$SCRIPT_DIR/test_startup_cleanup.sh"
run_bash_test "$SCRIPT_DIR/test_patient_flow.sh"

# --- PODSUMOWANIE ---
echo ""
echo -e "${BOLD}${CYAN}============================================${NC}"
if [[ $FAILED -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  WYNIK: $PASSED/$TOTAL PASSED ✓${NC}"
else
    echo -e "${BOLD}${RED}  WYNIK: $PASSED/$TOTAL PASSED, $FAILED FAILED ✗${NC}"
    for name in "${FAILED_NAMES[@]}"; do
        echo -e "  ${RED}  - $name${NC}"
    done
fi
echo -e "${BOLD}${CYAN}============================================${NC}"

exit $FAILED

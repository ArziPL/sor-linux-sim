#!/bin/bash
# run_tests.sh — uruchom wszystkie testy symulacji SOR
set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$(dirname "$SCRIPT_DIR")/build"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; NC='\033[0m'; BOLD='\033[1m'
PASSED=0; FAILED=0; TOTAL=0; FAILED_NAMES=()

echo -e "${BOLD}${CYAN}=== TESTY SYMULACJI SOR ===${NC}"
echo ""

# Build
echo -e "${YELLOW}[BUILD]${NC} Budowanie..."
cd "$BUILD_DIR" && cmake .. > /dev/null 2>&1 && make -j$(nproc) > /dev/null 2>&1
[[ $? -ne 0 ]] && echo -e "${RED}[BUILD] FAIL${NC}" && exit 1
echo -e "${GREEN}[BUILD]${NC} OK"
echo ""

cleanup() {
    pkill -9 -x dyrektor 2>/dev/null || true; pkill -9 -x lekarz 2>/dev/null || true
    pkill -9 -x pacjent 2>/dev/null || true; pkill -9 -x rejestracja 2>/dev/null || true
    pkill -9 -x generator 2>/dev/null || true; sleep 1
    ipcrm --all=shm 2>/dev/null || true; ipcrm --all=sem 2>/dev/null || true
    ipcrm --all=msg 2>/dev/null || true; sleep 0.5
}

run_test() {
    local script="$1" desc="$2"
    local name; name=$(basename "$script" .sh)
    TOTAL=$((TOTAL + 1))
    echo -e "  ${CYAN}▸${NC} ${BOLD}$name${NC}: $desc"

    cleanup
    local output rc
    output=$(bash "$script" 2>&1); rc=$?

    if [[ $rc -eq 0 ]]; then
        echo -e "    ${GREEN}✓ PASS${NC}"
        echo "$output" | grep "  OK:" | sed 's/^/      /'
        PASSED=$((PASSED + 1))
    else
        echo -e "    ${RED}✗ FAIL${NC}"
        echo "$output" | grep -E "(FAIL|OK):" | sed 's/^/      /'
        FAILED=$((FAILED + 1)); FAILED_NAMES+=("$name")
    fi
    echo ""
}

run_test "$SCRIPT_DIR/test_01_startup_cleanup.sh"   "Start i sprzątanie"
run_test "$SCRIPT_DIR/test_02_process_limit.sh"      "Limit procesów (-p 20)"
run_test "$SCRIPT_DIR/test_03_stress_no_deadlock.sh" "Stress: brak zakleszczenia"
run_test "$SCRIPT_DIR/test_04_kill_children.sh"      "Zabicie potomnych"

cleanup

echo -e "${BOLD}${CYAN}===========================${NC}"
if [[ $FAILED -eq 0 ]]; then
    echo -e "${BOLD}${GREEN}  WYNIK: $PASSED/$TOTAL PASSED ✓${NC}"
else
    echo -e "${BOLD}${RED}  WYNIK: $PASSED/$TOTAL PASSED, $FAILED FAILED ✗${NC}"
    for n in "${FAILED_NAMES[@]}"; do echo -e "  ${RED}  - $n${NC}"; done
fi
echo -e "${BOLD}${CYAN}===========================${NC}"
exit $FAILED

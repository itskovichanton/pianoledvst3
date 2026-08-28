#!/usr/bin/env bash
# Собирает и прогоняет все наборы тестов. Без CMake, одной командой.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$SCRIPT_DIR"

CXX="${CXX:-c++}"
BUILD_DIR="$(mktemp -d)"
ROOT=".."

FLAGS=(-std=c++17 -Wall -Wextra -Wpedantic
       -Iinclude -I"${ROOT}/shared" -I"${ROOT}/firmware/main" -Itests)
LIB=("${ROOT}/shared/led_protocol.c" src/frame_builder.cpp src/serial_port.cpp src/bridge.cpp)

# На macOS openpty живёт в libutil, но линковать её отдельно не нужно.
LINK=(-pthread)
if [[ "$(uname)" != "Darwin" ]]; then
    LINK+=(-lutil)
fi

echo "Сборка (${CXX}, C++17)..."
"${CXX}" "${FLAGS[@]}" "${ROOT}/shared/led_protocol.c" tests/test_protocol.cpp \
    -o "${BUILD_DIR}/protocol"
"${CXX}" "${FLAGS[@]}" "${LIB[@]}" tests/test_mac_side.cpp \
    -o "${BUILD_DIR}/mac_side" "${LINK[@]}"
"${CXX}" "${FLAGS[@]}" "${LIB[@]}" tests/test_serial_pty.cpp \
    -o "${BUILD_DIR}/serial" "${LINK[@]}"
"${CXX}" "${FLAGS[@]}" "${ROOT}/shared/led_protocol.c" "${ROOT}/firmware/main/led_guard.c" \
    tests/test_firmware_logic.cpp -o "${BUILD_DIR}/firmware"

FAILED=0
run_suite() {
    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    printf "║  %-56s║\n" "$1"
    echo "╚══════════════════════════════════════════════════════════╝"
    "${BUILD_DIR}/$2" || FAILED=1
}

run_suite "1/4  Протокол кадра (общий код обеих сторон)" protocol
run_suite "2/4  Mac: маска нот, кадр, отсутствие аллокаций" mac_side
run_suite "3/4  Транспорт через настоящий псевдотерминал" serial
run_suite "4/4  Логика прошивки: защита по току" firmware

echo ""
if [[ "${FAILED}" -eq 0 ]]; then
    echo "Все наборы пройдены."
else
    echo "Есть провалившиеся тесты — см. вывод выше."
fi
exit "${FAILED}"

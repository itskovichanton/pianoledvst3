#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$SCRIPT_DIR"

echo "════════════════════════════════════════"
echo "  Piano LED Strip (ESP32-C6) — Flash Tool"
echo "════════════════════════════════════════"

# ── 1. Загрузка окружения ESP-IDF ──
if ! command -v idf.py >/dev/null 2>&1; then
    echo "idf.py не найден в PATH, подтягиваю окружение..."
    if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
        . "${IDF_PATH}/export.sh"
    elif [[ -f "${HOME}/esp/esp-idf/export.sh" ]]; then
        . "${HOME}/esp/esp-idf/export.sh"
    fi
fi

if ! command -v idf.py >/dev/null 2>&1; then
    echo "Ошибка: idf.py не найден."
    echo "Выполни сначала:  source ~/esp/esp-idf/export.sh"
    exit 1
fi

# ── 2. Baud и порт (черновой; перед flash перечитываем) ──
BAUD="${BAUD:-460800}"

resolve_port() {
    if [[ -n "${ESPPORT:-}" ]]; then
        echo "${ESPPORT}"
        return
    fi
    ls /dev/cu.usb* 2>/dev/null | head -n1 || true
}

if [[ -n "$(resolve_port)" ]]; then
    if [[ -n "${ESPPORT:-}" ]]; then
        echo "  Порт (ESPPORT): ${ESPPORT}"
    else
        echo "  Порт (авто):    $(resolve_port)"
    fi
else
    echo "  Порт:           не найден (будет только сборка)"
fi
echo "  Baud:           ${BAUD}"
if [[ "${SKIP_BUILD:-0}" == "1" ]]; then
    echo "  SKIP_BUILD:     1 (только прошивка/монитор, без idf.py build)"
fi
if [[ "${RECONFIG:-0}" == "1" ]]; then
    echo "  RECONFIG:       1 (удалит sdkconfig → пересоздаст из sdkconfig.defaults)"
fi
if [[ "${FULLCLEAN:-0}" == "1" ]]; then
    echo "  FULLCLEAN:      1 (удалит build/ + sdkconfig → пересоздаст конфигурацию)"
fi
echo "════════════════════════════════════════"
echo ""

# ── 3. FULLCLEAN / RECONFIG ──────────────────────────────────
if [[ "${FULLCLEAN:-0}" == "1" ]]; then
    echo "FULLCLEAN: удаляю build/ и sdkconfig..."
    rm -rf build
    RECONFIG=1
fi

if [[ "${RECONFIG:-0}" == "1" ]]; then
    echo "RECONFIG: удаляю sdkconfig, пересоздаю конфигурацию из sdkconfig.defaults..."
    rm -f sdkconfig
    idf.py reconfigure
    echo ""
fi

# ── 4. Очистка (если build повреждён и FULLCLEAN не запускался) ──
if [[ -d "build" && ! -f "build/CMakeCache.txt" ]]; then
    echo "Каталог build повреждён, удаляю..."
    rm -rf build
fi

# ── 5. Сборка ──
run_build() {
    idf.py -b "${BAUD}" build "$@"
}

if [[ "${SKIP_BUILD:-0}" == "1" ]]; then
    if ! ls build/*.bin >/dev/null 2>&1; then
        echo "Ошибка: SKIP_BUILD=1, но в build/ нет .bin."
        echo "Собери один раз:  ./flash.sh   (без SKIP_BUILD)"
        exit 1
    fi
    echo "SKIP_BUILD=1 — пропускаю сборку, использую готовые build/*.bin"
    echo ""
else
    if ! run_build; then
        echo ""
        echo "Повторяю idf.py build (после полной очистки build/ это часто нужно один раз)..."
        run_build
    fi
fi

if [[ "${SKIP_FLASH:-0}" == "1" ]]; then
    echo "SKIP_FLASH=1 — прошивку не выполняю."
    exit 0
fi

# ── 6. Прошивка и монитор ────────────────────────────────────────
PORT="$(resolve_port)"
if [[ -z "${PORT}" ]]; then
    echo ""
    echo "Устройство не найдено."
    echo "  • Зажми BOOT → нажми RESET → отпусти BOOT → режим загрузчика"
    echo ""
    WAIT_SEC=90
    printf "Жду появления порта (до %d с)" "${WAIT_SEC}"
    for i in $(seq 1 "${WAIT_SEC}"); do
        PORT="$(resolve_port)"
        [[ -n "${PORT}" ]] && break
        sleep 1
        if (( i % 30 == 0 )); then
            printf ".\n"
            printf "  (осталось %d с)" "$(( WAIT_SEC - i ))"
        else
            printf "."
        fi
    done
    echo ""
    PORT="$(resolve_port)"
    if [[ -z "${PORT}" ]]; then
        echo "Устройство не появилось за ${WAIT_SEC} с. Запусти вручную:"
        echo "  ESPPORT=/dev/cu.usbXXX ./flash.sh"
        echo "или:  idf.py -p /dev/cu.usbXXX -b ${BAUD} flash monitor"
        exit 0
    fi
    echo "Порт найден: ${PORT}"
fi

echo ""
echo "Прошивка: ${PORT}"

run_flash() {
    idf.py -p "${PORT}" -b "$1" flash
}

if ! run_flash "${BAUD}"; then
    echo ""
    echo "Прошивка не удалась при baud=${BAUD}. Повтор с 115200…"
    if ! run_flash "115200"; then
        echo ""
        echo "Не удалось связаться с ROM-загрузчиком (типично: «No serial data received»)."
        echo "Что проверить:"
        echo "  • Закрой монитор, CLion Serial, другие терминалы с этим портом."
        echo "  • Убедись, что это плата ESP:  ls /dev/cu.usb*   и при необходимости  ESPPORT=… ./flash.sh"
        echo "  • Зажми BOOT, нажми RESET, отпусти RESET, через ~1 с отпусти BOOT — сразу запусти прошивку."
        echo "  • Другой USB-кабель / порт (не «только зарядка»)."
        exit 1
    fi
fi

# Монитор здесь НЕ запускается, и это намеренно: консоль в прошивке
# отключена (CONFIG_ESP_CONSOLE_NONE), по этому же порту идут бинарные кадры
# пикселей. idf.py monitor показал бы только нечитаемый поток и вдобавок занял
# бы порт, из-за чего плагин не смог бы подключиться.
#
# Чтобы проверить связь и погонять ленту, используй ledctl из mac/:
#   cd ../mac && ./run_ledctl.sh ping
#   cd ../mac && ./run_ledctl.sh scale
echo ""
echo "════════════════════════════════════════"
echo "  Прошито. Монитор не нужен — консоль отключена."
echo ""
echo "  Проверить ленту:"
echo "    cd ../mac && ./run_ledctl.sh ping"
echo "    cd ../mac && ./run_ledctl.sh scale"
echo "════════════════════════════════════════"
exit 0

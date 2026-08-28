#!/usr/bin/env bash
# Unsandboxed USB-мост для GarageBand. Держи запущенным, пока играет AU.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
APP="$HOME/Library/Application Support/PianoLED/PianoLEDBridge.app"
if [[ -d "$APP" ]]; then
    exec open -g "$APP"
fi
cd "$SCRIPT_DIR"
CXX="${CXX:-c++}"
BIN="$(mktemp -d)/ledbridged"
"${CXX}" -std=c++17 -Wall -Wextra -Wpedantic \
    -Iinclude -I../shared \
    ../shared/led_protocol.c src/frame_builder.cpp src/serial_port.cpp src/bridge.cpp \
    src/helper_app.mm tools/ledbridged.cpp \
    -pthread -framework Cocoa -o "${BIN}"
exec "${BIN}"

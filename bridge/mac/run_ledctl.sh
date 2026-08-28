#!/usr/bin/env bash
# Собирает и запускает ledctl — проверку ленты без Logic.
#
#   ./run_ledctl.sh ping
#   ./run_ledctl.sh scale --tempo-ms 200
#   ./run_ledctl.sh note 60 64 67
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
cd "$SCRIPT_DIR"

CXX="${CXX:-c++}"
BIN="$(mktemp -d)/ledctl"

"${CXX}" -std=c++17 -Wall -Wextra -Wpedantic \
    -Iinclude -I../shared \
    ../shared/led_protocol.c src/frame_builder.cpp src/serial_port.cpp src/bridge.cpp \
    tools/ledctl.cpp \
    -pthread -o "${BIN}"

exec "${BIN}" "$@"

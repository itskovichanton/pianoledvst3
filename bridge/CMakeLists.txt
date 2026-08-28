cmake_minimum_required(VERSION 3.16)
project(piano_led_bridge CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

# ── Библиотека Mac-стороны ──────────────────────────────────────────
# Ровно эти файлы добавляются в проект VST3-плагина. Зависимостей от JUCE нет
# намеренно: тот же код собирается здесь и проверяется тестами без хоста.
add_library(piano_led_bridge STATIC
    shared/led_protocol.c
    mac/src/frame_builder.cpp
    mac/src/serial_port.cpp
    mac/src/bridge.cpp
)
target_include_directories(piano_led_bridge PUBLIC mac/include shared)
target_compile_options(piano_led_bridge PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(piano_led_bridge PUBLIC Threads::Threads)

# ── Логика прошивки, собираемая нативно ─────────────────────────────
# led_guard.c уезжает на плату как есть, но зависимостей от ESP-IDF не имеет,
# поэтому проверяется обычными тестами на этой машине.
add_library(firmware_logic STATIC
    firmware/main/led_guard.c
    shared/led_protocol.c
)
target_include_directories(firmware_logic PUBLIC firmware/main shared)
target_compile_options(firmware_logic PRIVATE -Wall -Wextra -Wpedantic)

# ── Утилита проверки ленты ──────────────────────────────────────────
add_executable(ledctl mac/tools/ledctl.cpp)
target_link_libraries(ledctl PRIVATE piano_led_bridge)

# ── Тесты ───────────────────────────────────────────────────────────
add_executable(test_protocol mac/tests/test_protocol.cpp shared/led_protocol.c)
target_include_directories(test_protocol PRIVATE shared mac/tests)

add_executable(test_mac_side mac/tests/test_mac_side.cpp)
target_link_libraries(test_mac_side PRIVATE piano_led_bridge)
target_include_directories(test_mac_side PRIVATE mac/tests)

add_executable(test_serial_pty mac/tests/test_serial_pty.cpp)
target_link_libraries(test_serial_pty PRIVATE piano_led_bridge)
target_include_directories(test_serial_pty PRIVATE mac/tests)
if(NOT APPLE)
    target_link_libraries(test_serial_pty PRIVATE util)
endif()

add_executable(test_firmware_logic mac/tests/test_firmware_logic.cpp)
target_link_libraries(test_firmware_logic PRIVATE firmware_logic)
target_include_directories(test_firmware_logic PRIVATE mac/tests)

enable_testing()
add_test(NAME protocol COMMAND test_protocol)
add_test(NAME mac_side COMMAND test_mac_side)
add_test(NAME serial COMMAND test_serial_pty)
add_test(NAME firmware COMMAND test_firmware_logic)

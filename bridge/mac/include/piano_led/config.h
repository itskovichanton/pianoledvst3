#pragma once

#include "piano_led/frame_builder.h"

namespace piano_led {

/**
 * Цвет по умолчанию: красный на 2% яркости.
 *
 * 2% от 255 = 5.1, округляем к 5. Реальный цвет и яркость задаёт LedStyle
 * в настройках плагина; эта константа — запасной красный, если стиль ещё
 * не выставили. Прошивка режет канал сверху до 20% (51), а бюджет тока —
 * до 150 мА (см. led_guard.c).
 */
inline constexpr Rgb kNoteColor{5, 0, 0}; /* 5/255 ≈ 2%. */

/**
 * Локальный мост для GarageBand: AU в песочнице не видит /dev/cu.usbmodem*,
 * поэтому ходит сюда по TCP, а unsandboxed ledbridged открывает USB.
 */
inline constexpr const char* kBridgeTcpHost = "127.0.0.1";
inline constexpr int kBridgeTcpPort = 17321;

/** Unix-сокет и файловый канал: AU в GarageBand сидит в процессе без TCP. */
inline constexpr const char* kBridgeUnixPath = "/tmp/pianoled.sock";
inline constexpr const char* kBridgeDropDir = "/tmp/pianoled-bridge";

}  // namespace piano_led

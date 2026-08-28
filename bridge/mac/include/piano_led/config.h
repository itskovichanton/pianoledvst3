#pragma once

#include "piano_led/frame_builder.h"

namespace piano_led {

/**
 * Цвет горящей клавиши: красный на 1% яркости.
 *
 * 1% от 255 = 2.55, округляем к 3. Регулировки нет намеренно — яркость всегда
 * одна и та же. Если когда-нибудь понадобится другая, меняется здесь и только
 * здесь: и мост, и ledctl, и плагин берут цвет отсюда.
 *
 * По току: вся лента целиком (144 светодиода) на этой яркости берёт около
 * 34 мА при бюджете 150 мА. Плюс прошивка независимо считает ток каждого кадра
 * и притушила бы его, если бы он вышел за лимит (см. led_guard.c).
 */
inline constexpr Rgb kNoteColor{3, 0, 0}; /* 3/255 ≈ 1%. Прошивка тоже режет до 3. */

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

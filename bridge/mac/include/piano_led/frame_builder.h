#pragma once

#include <cstdint>
#include <vector>

#include "piano_led/note_bitmask.h"

namespace piano_led {

/** Цвет светодиода. */
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    constexpr Rgb() = default;
    constexpr Rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
        : r(red), g(green), b(blue) {}

    constexpr bool operator==(const Rgb& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
    constexpr bool operator!=(const Rgb& other) const { return !(*this == other); }
};

/**
 * Геометрия установки: как ноты ложатся на ленту.
 *
 * Раскладка здесь — арифметика, а не таблица: светодиоды идут подряд, клавиши
 * идут подряд, значит первый диод клавиши это (нота - lowestNote) * ledsPerKey.
 * Никакого файла раскладки не нужно.
 */
struct StripLayout {
    int ledCount = 144;    ///< светодиодов в ленте
    int ledsPerKey = 3;    ///< сколько диодов приходится на одну клавишу
    int lowestNote = 36;   ///< MIDI-номер самой левой клавиши (36 = C2)
    bool reversed = false; ///< true, если лента уложена справа налево

    /** Сколько клавиш помещается на ленту. */
    int keyCount() const { return ledCount / ledsPerKey; }

    /** MIDI-номер самой правой клавиши. */
    int highestNote() const { return lowestNote + keyCount() - 1; }

    /** Попадает ли нота на ленту. */
    bool covers(int note) const { return note >= lowestNote && note <= highestNote(); }

    /** Осмысленна ли геометрия. */
    bool isValid() const {
        return ledCount > 0 && ledsPerKey > 0 && ledCount >= ledsPerKey && lowestNote >= 0 &&
               highestNote() <= 127;
    }
};

/**
 * Собирает кадр пикселей из набора звучащих нот.
 *
 * Буфер кадра выделяется один раз в конструкторе, build() только пишет в него.
 * Вызывается из потока таймера, но аллокаций не делает намеренно: так же
 * пригодился бы и в аудио-потоке, если однажды понадобится.
 */
class FrameBuilder {
public:
    explicit FrameBuilder(StripLayout layout);

    const StripLayout& layout() const { return layout_; }

    /**
     * Заполняет кадр: гасит всё, затем зажигает диапазоны звучащих нот.
     * Ноты вне ленты молча пропускаются — MIDI-клавиатура шире метра ленты,
     * и это нормальная ситуация, а не ошибка.
     */
    void build(const NoteBitmask::Snapshot& notes, Rgb color);

    /** Готовый кадр: ledCount * 3 байта в порядке R, G, B. */
    const std::vector<std::uint8_t>& frame() const { return frame_; }

    /** Размер кадра в байтах. */
    std::size_t frameSize() const { return frame_.size(); }

    /**
     * Оценка тока кадра, мА. Прошивка считает то же самое и режет кадр, если он
     * не влезает в бюджет — здесь это нужно лишь чтобы показать цифру в UI.
     * ~20 мА на канал при полной яркости для WS2812B.
     */
    double estimatedCurrentMa() const;

private:
    StripLayout layout_;
    std::vector<std::uint8_t> frame_;
};

}  // namespace piano_led

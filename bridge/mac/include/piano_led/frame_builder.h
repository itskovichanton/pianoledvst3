#pragma once

#include <algorithm>
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
 * Внешний вид ленты: яркость в процентах и цвет (HSV).
 * toRgb() даёт каналы, которые уходят в кадр.
 */
struct LedStyle {
    float brightnessPercent = 2.0f; ///< 0.1…20, по умолчанию 2%. Ниже ~0.4% всё равно 1/255 — пол диода.
    float hue = 0.0f;               ///< 0…360
    float saturation = 1.0f;        ///< 0…1, 0 = белый

    Rgb toRgb() const;
};

/**
 * Геометрия установки: как ноты ложатся на ленту.
 *
 * Базовый случай — равномерный: (нота - lowestNote) * ledsPerKey + startLed.
 * Если keySizes не пуст, у каждой клавиши свой размер, и старт следующей —
 * сумма предыдущих. Так работает экран «Раскладка».
 */
struct StripLayout {
    int ledCount = 144;    ///< светодиодов в ленте
    int ledsPerKey = 3;    ///< размер по умолчанию, если keySizes пуст
    int lowestNote = 36;   ///< MIDI-номер первой клавиши (36 = C2)
    int startLed = 0;      ///< индекс первого диода первой клавиши
    bool reversed = false; ///< true, если лента уложена справа налево

    /** Размер каждой клавиши в диодах. Пустой вектор — все клавиши = ledsPerKey. */
    std::vector<std::uint8_t> keySizes;

    int keyCount() const {
        if (!keySizes.empty()) return static_cast<int>(keySizes.size());
        if (ledsPerKey <= 0) return 0;
        return ledCount / ledsPerKey;
    }

    int highestNote() const { return lowestNote + keyCount() - 1; }

    bool covers(int note) const { return note >= lowestNote && note <= highestNote(); }

    int sizeForKey(int keyIndex) const {
        if (keyIndex < 0 || keyIndex >= keyCount()) return 0;
        if (!keySizes.empty()) {
            const int value = static_cast<int>(keySizes[static_cast<std::size_t>(keyIndex)]);
            return value > 0 ? value : 1;
        }
        return ledsPerKey > 0 ? ledsPerKey : 0;
    }

    int ledStartForKey(int keyIndex) const {
        int led = startLed;
        for (int i = 0; i < keyIndex; ++i) led += sizeForKey(i);
        return led;
    }

    void makeSizesExplicit() {
        if (!keySizes.empty()) return;
        const int n = keyCount();
        const std::uint8_t fill =
            static_cast<std::uint8_t>(ledsPerKey > 0 ? ledsPerKey : 3);
        keySizes.assign(static_cast<std::size_t>(std::max(0, n)), fill);
    }

    void setMappedKeyCount(int n) {
        makeSizesExplicit();
        int maxKeys = 128 - lowestNote;
        if (maxKeys > 88) maxKeys = 88;
        if (maxKeys < 1) maxKeys = 1;
        if (n < 1) n = 1;
        if (n > maxKeys) n = maxKeys;
        std::uint8_t fill = static_cast<std::uint8_t>(ledsPerKey > 0 ? ledsPerKey : 3);
        if (fill < 1) fill = 1;
        if (static_cast<int>(keySizes.size()) < n)
            keySizes.resize(static_cast<std::size_t>(n), fill);
        else
            keySizes.resize(static_cast<std::size_t>(n));
    }

    void setKeySize(int keyIndex, int size) {
        makeSizesExplicit();
        if (keyIndex < 0 || keyIndex >= static_cast<int>(keySizes.size())) return;
        if (size < 1) size = 1;
        if (size > 16) size = 16;
        keySizes[static_cast<std::size_t>(keyIndex)] = static_cast<std::uint8_t>(size);
    }

    bool isValid() const {
        return ledCount > 0 && keyCount() > 0 && lowestNote >= 0 && highestNote() <= 127 &&
               startLed >= 0 && startLed < ledCount && (ledsPerKey > 0 || !keySizes.empty());
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

    /** Гасит все пиксели кадра. */
    void clear();

    /** Зажигает одну клавишу поверх текущего кадра — мигание в «Раскладке». */
    void lightNote(int midiNote, Rgb color);

    /** Зажигает один светодиод по индексу ленты (0 … ledCount-1). */
    void lightLed(int index, Rgb color);

    /**
     * Зажигает count диодов в середине ленты — превью цвета в «Настройках».
     * Не всю ленту: так меньше ток, а оттенок и яркость всё равно видны.
     */
    void lightCenter(int count, Rgb color);

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

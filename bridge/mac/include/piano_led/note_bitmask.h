#pragma once

#include <atomic>
#include <cstdint>

namespace piano_led {

/**
 * Какие MIDI-ноты звучат прямо сейчас — 128 бит в двух атомарных словах.
 *
 * ЭТО ЕДИНСТВЕННОЕ, ЧТО ТРОГАЕТ АУДИО-ПОТОК.
 *
 * processBlock() в VST3 — realtime-поток: если он опоздает, Logic выдаст щелчок
 * или дропаут. Поэтому там нельзя выделять память, брать мьютексы, трогать файлы
 * и ждать порт. Здесь ничего этого и нет: noteOn/noteOff — по одной атомарной
 * операции на событие, без единой аллокации. Тест test_mac_side проверяет это
 * буквально, подменяя глобальный operator new и считая вызовы.
 *
 * Читает маску другой поток (таймер, 120 Гц) — через snapshot(). Ему не нужен
 * согласованный срез обоих слов: если нота попала в следующий кадр вместо
 * текущего, это сдвиг на 8 мс, которого глазом не видно. Поэтому здесь
 * достаточно relaxed-операций, а не барьеров.
 */
class NoteBitmask {
public:
    /** Копия состояния на момент чтения. Тривиальная, копируется без аллокаций. */
    struct Snapshot {
        std::uint64_t low = 0;   ///< ноты 0..63
        std::uint64_t high = 0;  ///< ноты 64..127

        bool isOn(int note) const {
            if (note < 0 || note > 127) return false;
            const std::uint64_t word = (note < 64) ? low : high;
            return ((word >> (note & 63)) & 1ull) != 0;
        }

        int count() const { return popcount(low) + popcount(high); }

        bool operator==(const Snapshot& other) const {
            return low == other.low && high == other.high;
        }
        bool operator!=(const Snapshot& other) const { return !(*this == other); }

    private:
        static int popcount(std::uint64_t value) {
            int bits = 0;
            while (value != 0) {
                value &= value - 1;
                ++bits;
            }
            return bits;
        }
    };

    /** Зажечь ноту. Вызывается из аудио-потока. Ноты вне 0..127 игнорируются. */
    void noteOn(int note) {
        if (note < 0 || note > 127) return;
        words_[note >> 6].fetch_or(bit(note), std::memory_order_relaxed);
    }

    /** Погасить ноту. Вызывается из аудио-потока. */
    void noteOff(int note) {
        if (note < 0 || note > 127) return;
        words_[note >> 6].fetch_and(~bit(note), std::memory_order_relaxed);
    }

    /** Погасить всё — на All Notes Off, смену пресета, стоп транспорта. */
    void allNotesOff() {
        words_[0].store(0, std::memory_order_relaxed);
        words_[1].store(0, std::memory_order_relaxed);
    }

    /** Прочитать состояние. Вызывается из потока отправки. */
    Snapshot snapshot() const {
        Snapshot result;
        result.low = words_[0].load(std::memory_order_relaxed);
        result.high = words_[1].load(std::memory_order_relaxed);
        return result;
    }

private:
    static std::uint64_t bit(int note) { return 1ull << (note & 63); }

    /* Лежат в одной кеш-линии, но пишет их только аудио-поток, а читает только
     * таймер — ложного разделения между двумя писателями здесь не возникает. */
    std::atomic<std::uint64_t> words_[2] = {};
};

/* Атомарные операции над uint64 должны быть безадресными, иначе компилятор
 * подставит блокировку, и вызов из аудио-потока перестанет быть безопасным. */
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "atomic<uint64_t> должен быть lock-free — иначе аудио-поток может "
              "заблокироваться на мьютексе внутри атомарной операции");

}  // namespace piano_led

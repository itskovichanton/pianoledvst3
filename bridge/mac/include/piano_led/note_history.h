#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "piano_led/note_bitmask.h"

namespace piano_led {

/**
 * Последние note-on: кольцо фиксированного размера с меткой времени.
 *
 * push() зовёт аудио-поток — без аллокаций и без мьютексов. Читает UI/таймер.
 * Ёмкость N (окно) только ограничивает, сколько событий видно снаружи;
 * в кольце всегда лежат до kMax последних нажатий.
 */
class NoteHistory {
public:
    static constexpr int kMax = 128;
    static constexpr int kDefaultCapacity = 30;
    static constexpr int kDefaultChordWindowMs = 50;

    void push(int note) noexcept { pushAt(note, nowMs()); }

    /** Для тестов: явная метка времени, мс. */
    void pushAt(int note, std::int64_t timeMs) noexcept {
        if (note < 0 || note > 127) return;
        const auto index = write_.load(std::memory_order_relaxed);
        const auto slot = index % static_cast<std::uint32_t>(kMax);
        notes_[slot] = static_cast<std::uint8_t>(note);
        times_[slot] = timeMs;
        write_.store(index + 1u, std::memory_order_release);
    }

    void setCapacity(int n) {
        if (n < 1) n = 1;
        if (n > kMax) n = kMax;
        capacity_.store(n, std::memory_order_relaxed);
    }

    int capacity() const { return capacity_.load(std::memory_order_relaxed); }

    int size() const {
        const auto end = write_.load(std::memory_order_acquire);
        const int stored = end < static_cast<std::uint32_t>(kMax) ? static_cast<int>(end) : kMax;
        return std::min(stored, capacity());
    }

    /** Последние m note-on как маска (повторы одной высоты — один бит). */
    NoteBitmask::Snapshot asSnapshot(int m) const {
        NoteBitmask::Snapshot snap;
        if (m < 1) return snap;

        const auto end = write_.load(std::memory_order_acquire);
        const int stored = end < static_cast<std::uint32_t>(kMax) ? static_cast<int>(end) : kMax;
        const int take = std::min({m, capacity(), stored});
        for (int i = 0; i < take; ++i) {
            const auto index = (end - 1u - static_cast<std::uint32_t>(i)) %
                               static_cast<std::uint32_t>(kMax);
            snap.setOn(static_cast<int>(notes_[index]));
        }
        return snap;
    }

    /**
     * Последний аккорд: идём от самого свежего note-on назад, пока зазор
     * между соседними нажатиями не больше windowMs. Разрыв по времени —
     * граница аккорда.
     */
    NoteBitmask::Snapshot lastChordSnapshot(int windowMs) const {
        NoteBitmask::Snapshot snap;
        if (windowMs < 0) windowMs = 0;

        const auto end = write_.load(std::memory_order_acquire);
        const int stored = end < static_cast<std::uint32_t>(kMax) ? static_cast<int>(end) : kMax;
        const int visible = std::min(stored, capacity());
        if (visible <= 0) return snap;

        const auto slot = [&](int back) -> std::uint32_t {
            return (end - 1u - static_cast<std::uint32_t>(back)) %
                   static_cast<std::uint32_t>(kMax);
        };

        snap.setOn(static_cast<int>(notes_[slot(0)]));
        std::int64_t newer = times_[slot(0)];
        for (int back = 1; back < visible; ++back) {
            const auto index = slot(back);
            std::int64_t gap = newer - times_[index];
            if (gap < 0) gap = -gap;
            if (gap > static_cast<std::int64_t>(windowMs)) break;
            snap.setOn(static_cast<int>(notes_[index]));
            newer = times_[index];
        }
        return snap;
    }

private:
    static std::int64_t nowMs() noexcept {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    std::uint8_t notes_[kMax] = {};
    std::int64_t times_[kMax] = {};
    std::atomic<std::uint32_t> write_{0};
    std::atomic<int> capacity_{kDefaultCapacity};
};

}  // namespace piano_led

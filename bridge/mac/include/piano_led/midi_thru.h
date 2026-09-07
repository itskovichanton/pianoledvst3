#pragma once

#include <cstdint>

namespace piano_led {

/** Короткое MIDI-сообщение без SysEx — помещается в 3 байта, без аллокаций. */
struct MidiPacket {
    std::uint8_t bytes[3] {};
    std::uint8_t size = 0;

    bool operator== (const MidiPacket& other) const
    {
        if (size != other.size)
            return false;
        for (std::uint8_t i = 0; i < size; ++i)
            if (bytes[i] != other.bytes[i])
                return false;
        return true;
    }
};

/**
 * Что слать на внешний синтезатор.
 *
 * channel: 0 = Omni (оставить канал как в DAW), 1–16 = принудительно этот канал.
 * lowestNote / highestNote используются только при mappedKeysOnly.
 */
struct MidiThruConfig {
    int channel = 1;
    int lowestNote = 0;
    int highestNote = 127;
    bool mappedKeysOnly = false;
    bool sendSustain = true;
    bool sendPitchBend = false;
    bool sendModulation = false;
    bool sendProgramChange = false;
};

/**
 * Решает, уходит ли сообщение на синтезатор, и при необходимости
 * переписывает канал. Clock / SysEx / прочие CC отбрасываются.
 *
 * Realtime-безопасно: никаких аллокаций.
 */
inline bool filterMidi (const MidiThruConfig& cfg,
                        std::uint8_t status,
                        std::uint8_t d1,
                        std::uint8_t d2,
                        MidiPacket& out) noexcept
{
    if (status >= 0xF0)
        return false;

    const std::uint8_t type = static_cast<std::uint8_t> (status & 0xF0);
    std::uint8_t ch = static_cast<std::uint8_t> (status & 0x0F);
    if (cfg.channel >= 1 && cfg.channel <= 16)
        ch = static_cast<std::uint8_t> (cfg.channel - 1);

    switch (type)
    {
        case 0x80: /* note off */
        case 0x90: /* note on (velocity 0 = off) */
            if (d1 > 127)
                return false;
            if (cfg.mappedKeysOnly
                && (static_cast<int> (d1) < cfg.lowestNote
                    || static_cast<int> (d1) > cfg.highestNote))
                return false;
            out.bytes[0] = static_cast<std::uint8_t> (type | ch);
            out.bytes[1] = d1;
            out.bytes[2] = static_cast<std::uint8_t> (d2 & 0x7F);
            out.size = 3;
            return true;

        case 0xB0: /* CC */
        {
            const bool keep = (d1 == 64 && cfg.sendSustain)
                           || (d1 == 1 && cfg.sendModulation)
                           || d1 == 120
                           || d1 == 123;
            if (! keep)
                return false;
            out.bytes[0] = static_cast<std::uint8_t> (0xB0 | ch);
            out.bytes[1] = d1;
            out.bytes[2] = static_cast<std::uint8_t> (d2 & 0x7F);
            out.size = 3;
            return true;
        }

        case 0xE0: /* pitch bend */
            if (! cfg.sendPitchBend)
                return false;
            out.bytes[0] = static_cast<std::uint8_t> (0xE0 | ch);
            out.bytes[1] = static_cast<std::uint8_t> (d1 & 0x7F);
            out.bytes[2] = static_cast<std::uint8_t> (d2 & 0x7F);
            out.size = 3;
            return true;

        case 0xC0: /* program change */
            if (! cfg.sendProgramChange)
                return false;
            out.bytes[0] = static_cast<std::uint8_t> (0xC0 | ch);
            out.bytes[1] = static_cast<std::uint8_t> (d1 & 0x7F);
            out.size = 2;
            return true;

        default:
            return false;
    }
}

/**
 * All Notes Off + All Sound Off. channel 0 = все 16 каналов.
 * Пишет в out не больше maxOut пакетов, возвращает сколько записано.
 */
inline int panicPackets (int channel, MidiPacket* out, int maxOut) noexcept
{
    if (out == nullptr || maxOut <= 0)
        return 0;

    int n = 0;
    auto emit = [&] (int ch)
    {
        if (n + 2 > maxOut)
            return;
        out[n++] = { { static_cast<std::uint8_t> (0xB0 | ch), 123, 0 }, 3 };
        out[n++] = { { static_cast<std::uint8_t> (0xB0 | ch), 120, 0 }, 3 };
    };

    if (channel >= 1 && channel <= 16)
        emit (channel - 1);
    else
        for (int ch = 0; ch < 16; ++ch)
            emit (ch);

    return n;
}

} // namespace piano_led

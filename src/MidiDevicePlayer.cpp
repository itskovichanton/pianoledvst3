#include "MidiDevicePlayer.h"

namespace
{
juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}
} // namespace

MidiDevicePlayer::MidiDevicePlayer()
{
    listed_ = juce::MidiOutput::getAvailableDevices();
    startTimerHz (250);
}

MidiDevicePlayer::~MidiDevicePlayer()
{
    stop();
}

void MidiDevicePlayer::processMidi (const juce::MidiBuffer& midi) noexcept
{
    if (! running_.load (std::memory_order_relaxed))
        return;

    const auto cfg = snapshotConfig();

    for (const auto metadata : midi)
    {
        if (metadata.numBytes < 1 || metadata.numBytes > 3)
            continue;

        const auto* data = metadata.data;
        const auto d1 = metadata.numBytes > 1 ? data[1] : std::uint8_t { 0 };
        const auto d2 = metadata.numBytes > 2 ? data[2] : std::uint8_t { 0 };

        piano_led::MidiPacket packet;
        if (! piano_led::filterMidi (cfg, data[0], d1, d2, packet))
            continue;

        pushPacket (packet);
    }
}

void MidiDevicePlayer::setEnabled (bool on)
{
    const bool was = enabled_.exchange (on, std::memory_order_relaxed);
    if (on)
    {
        tryOpen();
        return;
    }

    if (was || output_ != nullptr)
    {
        sendPanicNow();
        closePort();
    }
}

void MidiDevicePlayer::setDevice (const juce::String& identifier, const juce::String& name)
{
    name_ = name;
    if (identifier_ == identifier)
    {
        if (enabled_.load (std::memory_order_relaxed) && output_ == nullptr && identifier.isNotEmpty())
            tryOpen();
        return;
    }

    if (output_ != nullptr)
        sendPanicNow();

    closePort();
    identifier_ = identifier;

    if (enabled_.load (std::memory_order_relaxed))
        tryOpen();
}

void MidiDevicePlayer::setConfig (const piano_led::MidiThruConfig& cfg)
{
    channel_.store (cfg.channel, std::memory_order_relaxed);
    lowest_.store (cfg.lowestNote, std::memory_order_relaxed);
    highest_.store (cfg.highestNote, std::memory_order_relaxed);
    flags_.store (packFlags (cfg), std::memory_order_relaxed);
}

piano_led::MidiThruConfig MidiDevicePlayer::config() const noexcept
{
    return snapshotConfig();
}

void MidiDevicePlayer::panic()
{
    sendPanicNow();
}

void MidiDevicePlayer::stop()
{
    enabled_.store (false, std::memory_order_relaxed);
    stopTimer();
    sendPanicNow();
    closePort();
}

juce::String MidiDevicePlayer::statusText() const
{
    if (! enabled_.load (std::memory_order_relaxed))
        return utf8 (u8"\u0441\u0438\u043d\u0442\u0435\u0437\u0430\u0442\u043e\u0440 \u0432\u044b\u043a\u043b");

    if (identifier_.isEmpty())
        return utf8 (u8"\u0443\u0441\u0442\u0440\u043e\u0439\u0441\u0442\u0432\u043e \u043d\u0435 \u0432\u044b\u0431\u0440\u0430\u043d\u043e");

    if (output_ != nullptr)
        return name_.isNotEmpty() ? name_ : identifier_;

    if (! deviceIsListed())
    {
        const auto label = name_.isNotEmpty() ? name_ : identifier_;
        return utf8 (u8"\u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d\u043e: ") + label;
    }

    return utf8 (u8"\u043d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043e\u0442\u043a\u0440\u044b\u0442\u044c");
}

juce::Array<juce::MidiDeviceInfo> MidiDevicePlayer::availableOutputs()
{
    return juce::MidiOutput::getAvailableDevices();
}

void MidiDevicePlayer::timerCallback()
{
    drainFifo();

    const auto now = juce::Time::currentTimeMillis();
    if (now >= nextRetryMs_)
    {
        nextRetryMs_ = now + 2000;
        listed_ = juce::MidiOutput::getAvailableDevices();
        if (enabled_.load (std::memory_order_relaxed) && output_ == nullptr && identifier_.isNotEmpty())
            tryOpen();
    }
}

void MidiDevicePlayer::tryOpen()
{
    if (! enabled_.load (std::memory_order_relaxed) || identifier_.isEmpty())
        return;

    if (output_ != nullptr)
        return;

    listed_ = juce::MidiOutput::getAvailableDevices();
    output_ = juce::MidiOutput::openDevice (identifier_);
    running_.store (output_ != nullptr, std::memory_order_relaxed);
    if (output_ == nullptr)
        nextRetryMs_ = juce::Time::currentTimeMillis() + 2000;
}

void MidiDevicePlayer::closePort()
{
    running_.store (false, std::memory_order_relaxed);
    discardFifo();
    output_.reset();
}

void MidiDevicePlayer::sendPanicNow()
{
    if (output_ == nullptr)
        return;

    piano_led::MidiPacket packets[32];
    const int n = piano_led::panicPackets (channel_.load (std::memory_order_relaxed),
                                           packets, 32);
    for (int i = 0; i < n; ++i)
        output_->sendMessageNow (juce::MidiMessage (packets[static_cast<std::size_t> (i)].bytes,
                                                    packets[static_cast<std::size_t> (i)].size));
}

void MidiDevicePlayer::drainFifo()
{
    if (output_ == nullptr)
    {
        if (fifo_.getNumReady() > 0)
            discardFifo();
        return;
    }

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToRead (fifo_.getNumReady(), start1, size1, start2, size2);

    auto sendSlice = [this] (int start, int size)
    {
        for (int i = 0; i < size; ++i)
        {
            const auto& packet = packets_[static_cast<std::size_t> (start + i)];
            if (packet.size == 0)
                continue;
            output_->sendMessageNow (juce::MidiMessage (packet.bytes, packet.size));
        }
    };

    sendSlice (start1, size1);
    sendSlice (start2, size2);
    fifo_.finishedRead (size1 + size2);
}

void MidiDevicePlayer::discardFifo()
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToRead (fifo_.getNumReady(), start1, size1, start2, size2);
    fifo_.finishedRead (size1 + size2);
}

void MidiDevicePlayer::pushPacket (const piano_led::MidiPacket& packet) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 < 1)
        return;

    packets_[static_cast<std::size_t> (start1)] = packet;
    fifo_.finishedWrite (1);
}

piano_led::MidiThruConfig MidiDevicePlayer::snapshotConfig() const noexcept
{
    piano_led::MidiThruConfig cfg;
    cfg.channel = channel_.load (std::memory_order_relaxed);
    cfg.lowestNote = lowest_.load (std::memory_order_relaxed);
    cfg.highestNote = highest_.load (std::memory_order_relaxed);
    const auto flags = flags_.load (std::memory_order_relaxed);
    cfg.mappedKeysOnly = (flags & kFlagMapped) != 0;
    cfg.sendSustain = (flags & kFlagSustain) != 0;
    cfg.sendPitchBend = (flags & kFlagPitch) != 0;
    cfg.sendModulation = (flags & kFlagMod) != 0;
    cfg.sendProgramChange = (flags & kFlagProg) != 0;
    return cfg;
}

std::uint32_t MidiDevicePlayer::packFlags (const piano_led::MidiThruConfig& cfg) noexcept
{
    std::uint32_t flags = 0;
    if (cfg.mappedKeysOnly) flags |= kFlagMapped;
    if (cfg.sendSustain) flags |= kFlagSustain;
    if (cfg.sendPitchBend) flags |= kFlagPitch;
    if (cfg.sendModulation) flags |= kFlagMod;
    if (cfg.sendProgramChange) flags |= kFlagProg;
    return flags;
}

bool MidiDevicePlayer::deviceIsListed() const
{
    if (identifier_.isEmpty())
        return false;

    for (const auto& info : listed_)
        if (info.identifier == identifier_)
            return true;

    return false;
}

#pragma once

#include "piano_led/midi_thru.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>

/**
 * MIDI с DAW на внешний синтезатор (CoreMIDI MidiOutput).
 *
 * Аудио-поток только кладёт короткие пакеты в lock-free fifo.
 * Открытие порта, Panic и sendMessageNow — на потоке таймера JUCE.
 */
class MidiDevicePlayer : private juce::Timer
{
public:
    MidiDevicePlayer();
    ~MidiDevicePlayer() override;

    void processMidi (const juce::MidiBuffer& midi) noexcept;

    void setEnabled (bool on);
    void setDevice (const juce::String& identifier, const juce::String& name);
    void setConfig (const piano_led::MidiThruConfig& cfg);
    piano_led::MidiThruConfig config() const noexcept;

    void panic();
    void stop();

    bool isEnabled() const noexcept { return enabled_.load (std::memory_order_relaxed); }
    bool isOpen() const noexcept { return running_.load (std::memory_order_relaxed); }
    juce::String deviceIdentifier() const { return identifier_; }
    juce::String deviceName() const { return name_; }
    juce::String statusText() const;

    static juce::Array<juce::MidiDeviceInfo> availableOutputs();

private:
    static constexpr int kFifoSize = 1024;
    static constexpr std::uint32_t kFlagMapped = 1u;
    static constexpr std::uint32_t kFlagSustain = 2u;
    static constexpr std::uint32_t kFlagPitch = 4u;
    static constexpr std::uint32_t kFlagMod = 8u;
    static constexpr std::uint32_t kFlagProg = 16u;

    void timerCallback() override;
    void tryOpen();
    void closePort();
    void sendPanicNow();
    void drainFifo();
    void discardFifo();
    void pushPacket (const piano_led::MidiPacket& packet) noexcept;
    piano_led::MidiThruConfig snapshotConfig() const noexcept;
    static std::uint32_t packFlags (const piano_led::MidiThruConfig& cfg) noexcept;
    bool deviceIsListed() const;

    std::unique_ptr<juce::MidiOutput> output_;
    juce::String identifier_;
    juce::String name_;
    std::atomic<bool> enabled_ { false };
    std::atomic<bool> running_ { false };
    std::atomic<int> channel_ { 1 };
    std::atomic<int> lowest_ { 0 };
    std::atomic<int> highest_ { 127 };
    std::atomic<std::uint32_t> flags_ { kFlagSustain };
    juce::AbstractFifo fifo_ { kFifoSize };
    std::array<piano_led::MidiPacket, kFifoSize> packets_ {};
    juce::Array<juce::MidiDeviceInfo> listed_;
    juce::int64 nextRetryMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDevicePlayer)
};

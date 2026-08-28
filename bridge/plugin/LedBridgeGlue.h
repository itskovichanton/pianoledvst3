#pragma once

/**
 * Прослойка между JUCE-плагином и лентой.
 *
 * Это единственный файл проекта, который знает про JUCE. Всё остальное —
 * обычный C++17, поэтому и тестируется без плагина и без хоста.
 *
 * ═══ КАК ВСТАВИТЬ В СВОЙ ПЛАГИН ═══
 *
 * 1. Добавь в проект плагина исходники:
 *      shared/led_protocol.c
 *      mac/src/frame_builder.cpp
 *      mac/src/serial_port.cpp
 *      mac/src/bridge.cpp
 *    и пути к заголовкам: mac/include, shared
 *
 * 2. В своём AudioProcessor заведи поле:
 *      piano_led::PluginLedBridge ledBridge;
 *
 * 3. В processBlock() — одна строка:
 *      ledBridge.processMidi (midiMessages);
 *
 * 4. В конструкторе процессора:  ledBridge.start();
 *    В деструкторе:              ledBridge.stop();
 *
 * Всё. Больше плагину знать ничего не нужно.
 *
 * ═══ ПОЧЕМУ ИМЕННО ТАК ═══
 *
 * processBlock — realtime-поток. Опоздание в нём слышно как щелчок или
 * дропаут в Logic. Поэтому оттуда нельзя:
 *   - выделять память (new, std::string, рост vector);
 *   - брать мьютексы;
 *   - трогать файлы и сокеты;
 *   - писать в последовательный порт.
 *
 * processMidi() не делает ничего из этого: он лишь переставляет биты в
 * атомарной маске. Кадр собирается и уходит в порт из juce::Timer — обычного
 * потока, которому опаздывать не страшно.
 *
 * Задержка от нажатия до света складывается так:
 *   размер буфера Logic (128 сэмплов при 48 кГц ≈ 2.7 мс)
 *   + период таймера (120 Гц ≈ до 8.3 мс)
 *   + USB CDC (меньше 1 мс)
 *   + вывод WS2812 на 144 диода (4.3 мс)
 *   ≈ 15 мс в худшем случае. Для визуальной подсветки это незаметно.
 */

#include <juce_audio_processors/juce_audio_processors.h>

#include "piano_led/bridge.h"

namespace piano_led {

/**
 * Мост, обёрнутый в таймер JUCE.
 *
 * Владеет фоновым потоком отправки. Аудио-поток трогает только processMidi().
 */
class PluginLedBridge : private juce::Timer {
public:
    /** Частота отправки кадров. 120 Гц — компромисс между задержкой и трафиком. */
    static constexpr int kFramesPerSecond = 120;

    /**
     * Как часто напоминать о себе, когда ноты не меняются.
     *
     * Прошивка гасит ленту, если хост молчит дольше двух секунд — это защита
     * от «Logic закрыли, а лента горит». Но длинная выдержанная нота как раз и
     * означает, что кадры не меняются и слать нечего. Поэтому раз в полсекунды
     * отправляем кадр принудительно.
     */
    static constexpr int kKeepaliveMs = 500;

    explicit PluginLedBridge(StripLayout layout = StripLayout{}) : bridge_(layout) {}

    ~PluginLedBridge() override { stop(); }

    /* ── АУДИО-ПОТОК ──────────────────────────────────────────────── */

    /**
     * Разбирает MIDI-буфер блока. Вызывать из processBlock().
     *
     * Realtime-безопасно: только атомарные операции, ни одной аллокации.
     */
    void processMidi(const juce::MidiBuffer& midi) noexcept {
        for (const auto metadata : midi) {
            const juce::MidiMessage message = metadata.getMessage();

            /* isNoteOn()/isNoteOff() с аргументами по умолчанию сами разбираются
             * с note-on velocity 0: многие клавиатуры гасят ноту именно так, и
             * наивная проверка isNoteOn() оставила бы её гореть навсегда. */
            if (message.isNoteOn()) {
                bridge_.noteOn(message.getNoteNumber());
            } else if (message.isNoteOff()) {
                bridge_.noteOff(message.getNoteNumber());
            } else if (message.isAllNotesOff() || message.isAllSoundOff()) {
                bridge_.allNotesOff();
            }
        }
    }

    /** Сбросить все ноты — на смену пресета, стоп транспорта, reset процессора. */
    void panic() noexcept { bridge_.allNotesOff(); }

    /* ── ПОТОК СООБЩЕНИЙ (конструктор, деструктор, UI) ────────────── */

    /**
     * Ищет ленту и запускает отправку кадров.
     *
     * @return false, если ленту не нашли. Плагин при этом продолжает работать
     *         как ни в чём не бывало — просто без подсветки. Отсутствие ленты
     *         не повод мешать человеку сводить трек.
     */
    bool start() {
        launchHelperIfNeeded();
        juce::Thread::sleep(700);
        std::string error;
        const bool connected = bridge_.openAuto(&error, 3000);
        if (!connected) lastError_ = error;

        startTimerHz(kFramesPerSecond);
        return connected;
    }

    /** Гасит ленту и останавливает отправку. */
    void stop() {
        stopTimer();
        if (bridge_.isOpen()) {
            bridge_.sendClear();
            bridge_.close();
        }
    }

    /** Повторная попытка подключения — например по кнопке в редакторе. */
    bool reconnect() {
        if (bridge_.isOpen()) bridge_.close();
        launchHelperIfNeeded();
        juce::Thread::sleep(700);
        std::string error;
        const bool connected = bridge_.openAuto(&error, 3000);
        if (!connected) lastError_ = error;
        return connected;
    }

    bool isConnected() const { return bridge_.isOpen(); }
    juce::String devicePath() const { return juce::String(bridge_.devicePath()); }
    juce::String lastError() const { return juce::String(lastError_); }

    /* Яркость не настраивается — она всегда 1%. Задана константой kNoteColor
     * в mac/include/piano_led/config.h. */

    /** Геометрия установки: длина ленты, диодов на клавишу, нижняя нота. */
    void setLayout(StripLayout layout) { bridge_.setLayout(layout); }
    const StripLayout& layout() const { return bridge_.layout(); }

    /** Последний собранный кадр — для предпросмотра ленты в редакторе плагина. */
    const std::vector<std::uint8_t>& lastFrame() const { return bridge_.lastFrame(); }

    /** Оценка тока, мА. Полезно показать в UI рядом с регулятором яркости. */
    double estimatedCurrentMa() const { return bridge_.estimatedCurrentMa(); }

private:
    void timerCallback() override {
        if (!bridge_.isOpen()) return;

        /* Кадр уходит только при смене нот. Раз в kKeepaliveMs отправляем
         * принудительно, чтобы watchdog прошивки не погасил выдержанный аккорд. */
        const juce::int64 now = juce::Time::currentTimeMillis();
        const bool keepalive = (now - lastSendMs_) >= kKeepaliveMs;

        const TickResult result = bridge_.tick(keepalive);

        if (result == TickResult::sent) {
            lastSendMs_ = now;
        } else if (result == TickResult::writeFailed) {
            /* Провод выдернули. Закрываем порт, чтобы следующий reconnect()
             * начал с чистого листа, и молчим — плагин должен продолжать
             * работать без ленты. */
            lastError_ = bridge_.lastError();
            bridge_.close();
        }
    }

    void launchHelperIfNeeded() {
        const juce::File sibling =
            juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                .getSiblingFile("ledbridged");
        if (sibling.existsAsFile())
            SerialPort::launchHelper(sibling.getFullPathName().toStdString());

        /* Из песочницы GarageBand posix_spawn наследует sandbox и снова не
         * увидит /dev. LaunchServices открывает отдельное .app без песочницы. */
        const juce::File helperApp =
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Library/Application Support/PianoLED/PianoLEDBridge.app");
        if (helperApp.isDirectory())
            juce::Process::openDocument(helperApp.getFullPathName(), juce::String());
    }

    LedBridge bridge_;
    juce::int64 lastSendMs_ = 0;
    std::string lastError_;
};

}  // namespace piano_led

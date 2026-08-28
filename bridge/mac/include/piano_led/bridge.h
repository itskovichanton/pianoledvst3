#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "led_protocol.h"
#include "piano_led/config.h"
#include "piano_led/frame_builder.h"
#include "piano_led/note_bitmask.h"
#include "piano_led/serial_port.h"

namespace piano_led {

/** Что произошло на последнем tick(). */
enum class TickResult {
    unchanged,   ///< ноты не менялись — кадр не отправляли
    sent,        ///< кадр ушёл на ленту
    notOpen,     ///< порт не открыт, отправлять некуда
    writeFailed  ///< запись не удалась, см. lastError()
};

const char* tickResultName(TickResult result);

/**
 * Мост между MIDI-событиями плагина и светодиодной лентой.
 *
 * Разделение потоков — главное, ради чего этот класс существует:
 *
 *   noteOn / noteOff / allNotesOff  — зовёт АУДИО-поток из processBlock().
 *       Внутри только атомарная операция над битовой маской. Ни аллокаций,
 *       ни блокировок, ни ввода-вывода.
 *
 *   tick / open / close / ping      — зовёт поток ТАЙМЕРА (juce::Timer, 120 Гц).
 *       Здесь уже можно всё: собрать кадр, закодировать, записать в порт.
 *
 * Смешивать нельзя: вызов tick() из аудио-потока сведёт на нет весь смысл.
 */
class LedBridge {
public:
    explicit LedBridge(StripLayout layout = StripLayout{});

    /* ── Вызывается из АУДИО-потока ───────────────────────────────── */

    void noteOn(int midiNote) { notes_.noteOn(midiNote); }
    void noteOff(int midiNote) { notes_.noteOff(midiNote); }
    void allNotesOff() { notes_.allNotesOff(); }

    /* ── Вызывается из потока таймера ─────────────────────────────── */

    /** Открывает порт по явному пути. */
    bool open(const std::string& devicePath, std::string* error = nullptr);

    /**
     * Ищет устройство сам: перебирает /dev/cu.* и берёт первое, что ответило
     * на PING. Так лента не перепутается с другим USB-устройством.
     * @param error текст с перечнем того, что пробовали.
     */
    /**
     * @param tryTcp если serial-портов нет (песочница GarageBand) — пробуем
     *               localhost helper. У самого helper этот флаг должен быть false,
     *               иначе он подключится сам к себе.
     */
    bool openAuto(std::string* error = nullptr, int replyTimeoutMs = 2000,
                  bool tryTcp = true, int helperAttempts = 10);

    /**
     * Повторяет PING, пока плата не ответит или не истечёт timeoutMs.
     * Нужно потому, что открытие /dev/cu.usbmodem* на C6 часто сбрасывает чип,
     * и первые сотни миллисекунд прошивка ещё не слушает USB.
     */
    bool probe(int timeoutMs = 2000);

    /**
     * Берёт под управление уже открытый дескриптор вместо пути к устройству.
     * Нужно тестам, чтобы подставить псевдотерминал на место ленты и прогнать
     * весь путь до байтов на проводе, не имея железа под рукой.
     */
    void adoptPortFd(int fd, std::string label = "<fd>");

    void close() { port_.close(); }
    bool isOpen() const { return port_.isOpen(); }
    const std::string& devicePath() const { return port_.path(); }

    /**
     * Собирает кадр из текущих нот и отправляет, ЕСЛИ ноты изменились.
     * Вызывать с частотой таймера. Возврат unchanged — это норма, а не ошибка:
     * при неподвижных руках по проводу не идёт ничего.
     *
     * @param force отправить кадр даже без изменений (например для keepalive,
     *              чтобы watchdog прошивки не погасил ленту).
     */
    TickResult tick(bool force = false);

    /** Гасит ленту немедленно, отдельным кадром. Звать при закрытии плагина. */
    bool sendClear(std::string* error = nullptr);

    /**
     * Проверяет связь: шлёт PING и ждёт PONG.
     * @param ledCountOut если не null, туда попадёт длина ленты из ответа.
     */
    bool ping(int* ledCountOut = nullptr, std::string* error = nullptr, int timeoutMs = 400);

    /** Забирает накопившиеся LOG-кадры от прошивки. Опустошает очередь. */
    std::vector<std::string> takeLogs();

    /* ── Настройки и состояние ────────────────────────────────────── */

    const StripLayout& layout() const { return builder_.layout(); }
    void setLayout(StripLayout layout);

    /** Нота, которую «Раскладка» мигает на ленте. -1 — выключено. */
    void setPreviewNote(int midiNote, bool lit);

    /** Один диод для теста «бегущий огонь». -1 — выключено. */
    void setChaseLed(int ledIndex, bool lit);

    /** Оценка тока последнего собранного кадра, мА. */
    double estimatedCurrentMa() const { return builder_.estimatedCurrentMa(); }

    /** Последний собранный кадр — для отрисовки предпросмотра в UI плагина. */
    const std::vector<std::uint8_t>& lastFrame() const { return builder_.frame(); }

    NoteBitmask::Snapshot activeNotes() const { return notes_.snapshot(); }

    const std::string& lastError() const { return lastError_; }

private:
    /** Читает то, что пришло от прошивки, и раскладывает по типам кадров. */
    void pump();

    /** Ждёт кадр нужного типа не дольше timeoutMs. */
    bool waitForFrame(std::uint8_t wantedType, std::vector<std::uint8_t>* payloadOut,
                      int timeoutMs);

    bool sendFrame(std::uint8_t type, const std::uint8_t* payload, std::uint16_t length,
                   std::string* error);

    NoteBitmask notes_;
    FrameBuilder builder_;
    SerialPort port_;

    NoteBitmask::Snapshot lastSent_;
    bool everSent_ = false;

    std::vector<std::uint8_t> outBuffer_;
    std::uint8_t readBuffer_[512] = {};
    led_proto_decoder_t decoder_{};

    std::vector<std::string> logs_;
    std::vector<std::uint8_t> pendingPong_;
    bool havePong_ = false;

    std::string lastError_;

    int previewNote_ = -1;
    bool previewLit_ = false;

    int chaseLed_ = -1;
    bool chaseLit_ = false;
};

}  // namespace piano_led

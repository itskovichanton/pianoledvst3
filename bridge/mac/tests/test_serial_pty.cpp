/**
 * Тесты SerialPort и LedBridge через настоящий псевдотерминал.
 *
 * Железа в тестовой среде нет, но openpty() даёт пару настоящих файловых
 * дескрипторов с настоящим termios — то есть проверяется реальный код
 * записи/чтения, а не заглушка. Один конец берёт SerialPort, на другом
 * сидит эмулятор прошивки, разбирающий кадры тем же led_protocol.c,
 * который будет крутиться на ESP32.
 *
 * Так путь «маска нот -> кадр -> провод -> пиксели» проверяется целиком.
 */

#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "led_protocol.h"
#include "piano_led/bridge.h"
#include "piano_led/serial_port.h"
#include "test_framework.h"

using namespace piano_led;
using namespace test_framework;

namespace {

/**
 * Эмулятор прошивки на дальнем конце провода: читает байты, собирает кадры
 * тем же декодером, что и ESP32, и отвечает на PING.
 */
class FakeFirmware {
public:
    explicit FakeFirmware(int fd) : fd_(fd) { led_proto_decoder_init(&decoder_); }

    struct Frame {
        std::uint8_t type = 0;
        std::vector<std::uint8_t> payload;
    };

    /** Читает всё доступное, разбирает и отвечает на PING. */
    void poll() {
        std::uint8_t buffer[512];
        for (;;) {
            const ssize_t n = ::read(fd_, buffer, sizeof(buffer));
            if (n <= 0) return;
            for (ssize_t i = 0; i < n; ++i) {
                if (!led_proto_decoder_push(&decoder_, buffer[i])) continue;

                Frame frame;
                frame.type = decoder_.type;
                frame.payload.assign(decoder_.payload, decoder_.payload + decoder_.length);
                received.push_back(frame);

                if (frame.type == LED_FRAME_PING) sendPong(144);
            }
        }
    }

    /** Ждёт, пока придёт хотя бы count кадров, но не дольше timeoutMs. */
    bool waitFor(std::size_t count, int timeoutMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            poll();
            if (received.size() >= count) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        poll();
        return received.size() >= count;
    }

    void sendPong(int ledCount) {
        const std::uint8_t payload[LED_PROTO_PONG_PAYLOAD_SIZE] = {
            LED_PROTO_VERSION, static_cast<std::uint8_t>(ledCount & 0xFF),
            static_cast<std::uint8_t>((ledCount >> 8) & 0xFF)};
        std::uint8_t out[16];
        const std::size_t size =
            led_proto_encode(LED_FRAME_PONG, payload, sizeof(payload), out, sizeof(out));
        ssize_t ignored = ::write(fd_, out, size);
        (void)ignored;
    }

    void sendLog(const std::string& text) {
        std::uint8_t out[256];
        const std::size_t size = led_proto_encode(
            LED_FRAME_LOG, reinterpret_cast<const std::uint8_t*>(text.data()),
            static_cast<std::uint16_t>(text.size()), out, sizeof(out));
        ssize_t ignored = ::write(fd_, out, size);
        (void)ignored;
    }

    /** Пишет сырые байты — чтобы сымитировать мусор от ROM-загрузчика. */
    void sendRaw(const std::string& text) {
        ssize_t ignored = ::write(fd_, text.data(), text.size());
        (void)ignored;
    }

    std::vector<Frame> received;

private:
    int fd_;
    led_proto_decoder_t decoder_{};
};

/** Пара соединённых дескрипторов: один «плагину», второй «прошивке». */
struct PtyPair {
    int host = -1;
    int device = -1;

    bool create() {
        if (::openpty(&host, &device, nullptr, nullptr, nullptr) != 0) return false;
        ::fcntl(host, F_SETFL, O_NONBLOCK);
        ::fcntl(device, F_SETFL, O_NONBLOCK);

        /* Псевдотерминал стартует в каноническом режиме: дисциплина линии
         * копит байты до перевода строки и правит их по дороге. Настоящий
         * USB-CDC так себя не ведёт, поэтому приводим дальний конец к тому же
         * сырому режиму, в который SerialPort переводит ближний. */
        SerialPort::configureRaw(device, nullptr);
        return true;
    }

    void closeDevice() {
        if (device >= 0) {
            ::close(device);
            device = -1;
        }
    }
};

std::string litLeds(const std::vector<std::uint8_t>& frame) {
    std::string out;
    for (std::size_t led = 0; led * 3 + 2 < frame.size(); ++led) {
        const bool on = frame[led * 3] != 0 || frame[led * 3 + 1] != 0 || frame[led * 3 + 2] != 0;
        if (!on) continue;
        if (!out.empty()) out += ",";
        out += std::to_string(led);
    }
    return out;
}

/* ══════════════════════ SerialPort ══════════════════════ */

void test_port_write_read() {
    begin_test("SerialPort — запись и чтение через псевдотерминал");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }

    SerialPort port;
    port.adoptFd(pty.host, "<pty>");
    check(port.isOpen(), "порт открыт");

    const std::uint8_t payload[5] = {1, 2, 3, 4, 5};
    std::string error;
    check(port.writeAll(payload, sizeof(payload), &error), "writeAll успешен (" + error + ")");

    std::uint8_t buffer[32] = {};
    std::ptrdiff_t got = 0;
    for (int attempt = 0; attempt < 100 && got < 5; ++attempt) {
        const ssize_t n = ::read(pty.device, buffer + got, sizeof(buffer) - got);
        if (n > 0) got += n;
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    check_eq(got, std::ptrdiff_t{5}, "на другом конце получено 5 байт");
    check(buffer[0] == 1 && buffer[4] == 5, "байты не искажены");

    port.close();
    check(!port.isOpen(), "после close порт закрыт");
    pty.closeDevice();
}

void test_port_errors() {
    begin_test("SerialPort — ошибки");

    SerialPort port;
    std::string error;

    check(!port.open("/dev/такого-порта-нет", &error), "открытие несуществующего пути отвергнуто");
    check(!error.empty(), "текст ошибки заполнен");
    check(!port.isOpen(), "порт остался закрытым");

    const std::uint8_t byte = 1;
    check(!port.writeAll(&byte, 1, &error), "запись в закрытый порт отвергнута");

    std::uint8_t buffer[4];
    check_eq(port.readSome(buffer, sizeof(buffer)), std::ptrdiff_t{-1},
             "чтение из закрытого порта -> -1");
}

void test_port_candidate_listing() {
    begin_test("SerialPort — перечисление кандидатов");

    /* В песочнице устройств нет, но вызов обязан отработать без падения
     * и вернуть пустой список, а не мусор. */
    const std::vector<std::string> candidates = SerialPort::listCandidates();
    for (const std::string& path : candidates) {
        check(path.rfind("/dev/cu.", 0) == 0, "кандидат " + path + " начинается с /dev/cu.");
    }
    check(true, "перечисление отработало, найдено кандидатов: " +
                    std::to_string(candidates.size()));
}

/* ══════════════════════ LedBridge через провод ══════════════════════ */

void test_bridge_sends_pixels() {
    begin_test("LedBridge -> провод -> прошивка: кадр пикселей");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }
    FakeFirmware firmware(pty.device);

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(pty.host);

    bridge.noteOn(60);
    const TickResult result = bridge.tick();
    check_eq(std::string(tickResultName(result)), std::string("sent"), "кадр отправлен");

    check(firmware.waitFor(1, 500), "прошивка получила кадр");
    if (!firmware.received.empty()) {
        const FakeFirmware::Frame& frame = firmware.received.front();
        check_eq(int(frame.type), int(LED_FRAME_PIXELS), "тип кадра PIXELS");
        check_eq(frame.payload.size(), std::size_t{432}, "144 светодиода * 3 байта");
        check_eq(litLeds(frame.payload), std::string("72,73,74"),
                 "у прошивки горят ровно те диоды, что задумал плагин");
        check_eq(int(frame.payload[72 * 3]), 3, "яркость 1% доехала без искажений");
    }

    pty.closeDevice();
}

void test_bridge_ping_pong() {
    begin_test("LedBridge — PING/PONG находит устройство");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }
    FakeFirmware firmware(pty.device);

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(pty.host);

    /* ping() блокируется в ожидании ответа, а отвечать должна «прошивка» —
     * значит её нужно крутить в соседнем потоке. */
    std::thread responder([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        while (std::chrono::steady_clock::now() < deadline) {
            firmware.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    int ledCount = 0;
    std::string error;
    const bool answered = bridge.ping(&ledCount, &error, 500);
    responder.join();

    check(answered, "прошивка ответила на PING (" + error + ")");
    check_eq(ledCount, 144, "из PONG прочитана длина ленты");

    pty.closeDevice();
}

void test_bridge_receives_logs() {
    begin_test("LedBridge — приём LOG-кадров от прошивки");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }
    FakeFirmware firmware(pty.device);

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(pty.host);

    firmware.sendLog("strip ready: 144 leds");
    firmware.sendLog("current clamp active");

    std::vector<std::string> logs;
    for (int attempt = 0; attempt < 100 && logs.size() < 2; ++attempt) {
        std::vector<std::string> chunk = bridge.takeLogs();
        logs.insert(logs.end(), chunk.begin(), chunk.end());
        if (logs.size() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    check_eq(logs.size(), std::size_t{2}, "получены оба сообщения");
    if (logs.size() == 2) {
        check_eq(logs[0], std::string("strip ready: 144 leds"), "первое сообщение целое");
        check_eq(logs[1], std::string("current clamp active"), "второе сообщение целое");
    }

    pty.closeDevice();
}

void test_bridge_survives_boot_garbage() {
    begin_test("LedBridge — мусор от ROM-загрузчика не ломает связь");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }
    FakeFirmware firmware(pty.device);

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(pty.host);

    /* Плата только что включилась: ROM печатает свой баннер в тот же USB. */
    firmware.sendRaw("ESP-ROM:esp32c6-20220919\r\nBuild:Sep 19 2022\r\nrst:0x1\r\n");
    firmware.sendLog("после мусора связь жива");

    std::vector<std::string> logs;
    for (int attempt = 0; attempt < 100 && logs.empty(); ++attempt) {
        logs = bridge.takeLogs();
        if (logs.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    check_eq(logs.size(), std::size_t{1}, "кадр после баннера загрузчика распознан");
    if (!logs.empty()) {
        check_eq(logs[0], std::string("после мусора связь жива"), "содержимое не пострадало");
    }

    pty.closeDevice();
}

void test_bridge_clear() {
    begin_test("LedBridge — гашение ленты");

    PtyPair pty;
    if (!pty.create()) {
        check(false, "не удалось создать псевдотерминал");
        return;
    }
    FakeFirmware firmware(pty.device);

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(pty.host);

    bridge.noteOn(60);
    bridge.tick();
    check(firmware.waitFor(1, 500), "кадр с нотой ушёл");

    std::string error;
    check(bridge.sendClear(&error), "sendClear успешен (" + error + ")");
    check(firmware.waitFor(2, 500), "кадр CLEAR получен");

    if (firmware.received.size() >= 2) {
        check_eq(int(firmware.received[1].type), int(LED_FRAME_CLEAR), "тип кадра CLEAR");
        check_eq(firmware.received[1].payload.size(), std::size_t{0}, "CLEAR без payload");
    }
    check_eq(bridge.activeNotes().count(), 0, "после CLEAR звучащих нот не осталось");

    pty.closeDevice();
}

void test_bridge_reports_write_failure() {
    begin_test("LedBridge — ошибка записи доходит до вызывающего");

    /* Оговорка про то, чего здесь НЕ проверяется: настоящее выдёргивание USB
     * псевдотерминалом не воспроизводится. На Linux запись в мастер после
     * закрытия слейва молча проглатывается (проверено отдельной пробой:
     * write() возвращает полную длину и errno == 0), на macOS даёт EIO.
     * Поэтому проверяем не обрыв как таковой, а что любая ошибка записи
     * превращается в writeFailed и попадает в lastError(), а не теряется. */

    const int readOnly = ::open("/dev/null", O_RDONLY);
    if (readOnly < 0) {
        check(false, "не удалось открыть /dev/null");
        return;
    }

    LedBridge bridge(StripLayout{});
    bridge.adoptPortFd(readOnly, "<только для чтения>");

    bridge.noteOn(60);
    const TickResult result = bridge.tick();

    check_eq(std::string(tickResultName(result)), std::string("writeFailed"),
             "запись в непригодный для записи порт -> writeFailed");
    check(!bridge.lastError().empty(), "текст ошибки заполнен: " + bridge.lastError());

    /* И мост не считает такой кадр отправленным — следующая попытка повторит его. */
    bridge.noteOn(64);
    check_eq(std::string(tickResultName(bridge.tick())), std::string("writeFailed"),
             "следующая попытка снова честно сообщает об ошибке");
}

}  // namespace

int main() {
    std::cout << "Тесты транспорта через настоящий псевдотерминал\n";

    test_port_write_read();
    test_port_errors();
    test_port_candidate_listing();

    test_bridge_sends_pixels();
    test_bridge_ping_pong();
    test_bridge_receives_logs();
    test_bridge_survives_boot_garbage();
    test_bridge_clear();
    test_bridge_reports_write_failure();

    return finish();
}

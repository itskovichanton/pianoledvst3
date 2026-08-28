#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace piano_led {

/**
 * Последовательный порт через POSIX termios.
 *
 * Своего класса порта в JUCE нет, а тянуть библиотеку ради open/write незачем.
 *
 * Про macOS и ESP32-C6:
 *   - Открывать нужно /dev/cu.*, а НЕ /dev/tty.*: tty-вариант ждёт сигнала DCD
 *     и на USB-устройстве просто виснет на open().
 *   - У C6 порт USB Serial/JTAG, то есть USB CDC. Скорость там фиктивная и
 *     драйвером игнорируется — упереться в baud нельзя, кадр 432 байта уходит
 *     за доли миллисекунды.
 *
 * Класс не потокобезопасен: им владеет один поток отправки. Из аудио-потока
 * его трогать нельзя ни при каких обстоятельствах — write() может заблокироваться.
 */
class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /**
     * Открывает порт в сыром неблокирующем режиме.
     * @return false при ошибке, тогда error содержит текст с errno.
     */
    bool open(const std::string& path, std::string* error = nullptr);

    /**
     * TCP к локальному ledbridged. Нужен GarageBand: песочница AU не пускает
     * в /dev/cu.*, а исходящий localhost обычно разрешён.
     */
    bool openTcp(const std::string& host, int port, std::string* error = nullptr);

    /**
     * Берёт под управление уже открытый дескриптор — так тесты подсовывают
     * псевдотерминал вместо железа. Порт становится владельцем fd.
     *
     * К дескриптору применяется та же настройка сырого режима, что и в open():
     * иначе тест проверял бы не тот путь, каким пойдут данные в бою. Если fd —
     * не терминал (труба, сокет), настройка молча пропускается.
     */
    void adoptFd(int fd, std::string path = "<fd>");

    /**
     * Переводит дескриптор в сырой неканонический режим.
     *
     * Без этого дисциплина линии копит байты до перевода строки и переводит
     * \r в \n — бинарные кадры при этом либо застревают, либо приезжают
     * искажёнными. Публичный, потому что тестам нужно применить то же самое
     * к дальнему концу псевдотерминала.
     *
     * @return false, если fd не терминал.
     */
    static bool configureRaw(int fd, std::string* error = nullptr);

    void close();
    bool isOpen() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    /**
     * Пишет все байты, повторяя попытку при EAGAIN и EINTR.
     * @param timeoutMs потолок ожидания; 0 — одна попытка без ожидания.
     * @return false, если за отведённое время ушло не всё.
     */
    bool writeAll(const std::uint8_t* data, std::size_t size, std::string* error = nullptr,
                  int timeoutMs = 200);

    /**
     * Читает то, что уже пришло, не блокируясь.
     * @return число прочитанных байт, 0 если пусто, -1 при ошибке.
     */
    std::ptrdiff_t readSome(std::uint8_t* buffer, std::size_t capacity);

    /**
     * Кандидаты на подключение: /dev/cu.usbmodem*, /dev/cu.usbserial*, /dev/cu.SLAB*.
     * Отсортированы по имени. Что именно из них наше — покажет PING/PONG.
     */
    static std::vector<std::string> listCandidates();

    int nativeFd() const { return fd_; }

    /** Запускает unsandboxed helper и сразу возвращается. Если уже слушает — ок. */
    static bool launchHelper(const std::string& executablePath, std::string* error = nullptr);

private:
    int fd_ = -1;
    std::string path_;
};

}  // namespace piano_led

#include "piano_led/serial_port.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

extern char** environ;

namespace piano_led {
namespace {

void setError(std::string* error, const std::string& text) {
    if (error != nullptr) *error = text;
}

std::string errnoText(const std::string& prefix) {
    return prefix + ": " + std::string(std::strerror(errno)) + " (errno " +
           std::to_string(errno) + ")";
}

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& path, std::string* error) {
    close();

    /* O_NONBLOCK на open обязателен: без него open() на /dev/cu.* может уснуть
     * навсегда, ожидая модемные сигналы. O_NOCTTY — чтобы порт не стал
     * управляющим терминалом процесса. */
    const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        setError(error, errnoText("не удалось открыть " + path));
        return false;
    }

    if (!configureRaw(fd, error)) {
        ::close(fd);
        return false;
    }

    ::tcflush(fd, TCIOFLUSH);

    fd_ = fd;
    path_ = path;
    setError(error, "");
    return true;
}

bool SerialPort::configureRaw(int fd, std::string* error) {
    termios options{};
    if (::tcgetattr(fd, &options) != 0) {
        setError(error, errnoText("tcgetattr"));
        return false;
    }

    /* Сырой режим: никакой обработки символов, эха и трансляции переводов
     * строки. Без него дисциплина линии держит данные в буфере до \n и
     * подменяет байты — бинарный кадр этого не переживёт. */
    ::cfmakeraw(&options);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);  // аппаратного потока нет
    options.c_cflag &= ~static_cast<tcflag_t>(HUPCL);    // не дёргать DTR при close — C6 из-за этого ресетится
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    /* Для USB CDC скорость игнорируется, но выставить её нужно: иначе на части
     * систем termios отдаёт B0 и порт ведёт себя как отключённый. */
    ::cfsetispeed(&options, B115200);
    ::cfsetospeed(&options, B115200);

    if (::tcsetattr(fd, TCSANOW, &options) != 0) {
        setError(error, errnoText("tcsetattr"));
        return false;
    }

    setError(error, "");
    return true;
}

void SerialPort::adoptFd(int fd, std::string path) {
    close();
    fd_ = fd;
    path_ = std::move(path);

    /* Тот же сырой режим, что и в open(). Если это не терминал — не беда,
     * трубе и сокету дисциплина линии не мешает. */
    configureRaw(fd, nullptr);
}

void SerialPort::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
}

bool SerialPort::writeAll(const std::uint8_t* data, std::size_t size, std::string* error,
                          int timeoutMs) {
    if (fd_ < 0) {
        setError(error, "порт не открыт");
        return false;
    }
    if (size == 0) return true;

    const std::int64_t deadline = nowMs() + timeoutMs;
    std::size_t written = 0;

    while (written < size) {
        const ssize_t n = ::write(fd_, data + written, size - written);

        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && errno == EINTR) continue;  // сигнал прервал — просто повторяем

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Буфер драйвера полон. Ждём готовности к записи, а не крутим
             * процессор впустую. */
            const std::int64_t remaining = deadline - nowMs();
            if (remaining <= 0) {
                setError(error, "таймаут записи: ушло " + std::to_string(written) + " из " +
                                    std::to_string(size) + " байт");
                return false;
            }

            pollfd waiter{};
            waiter.fd = fd_;
            waiter.events = POLLOUT;
            const int ready = ::poll(&waiter, 1, static_cast<int>(remaining));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) {
                setError(error, "таймаут ожидания готовности порта " + path_);
                return false;
            }
            continue;
        }

        setError(error, errnoText("ошибка записи в " + path_));
        return false;
    }

    setError(error, "");
    return true;
}

std::ptrdiff_t SerialPort::readSome(std::uint8_t* buffer, std::size_t capacity) {
    if (fd_ < 0 || buffer == nullptr || capacity == 0) return -1;

    const ssize_t n = ::read(fd_, buffer, capacity);
    if (n >= 0) return static_cast<std::ptrdiff_t>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

std::vector<std::string> SerialPort::listCandidates() {
    std::vector<std::string> found;

    DIR* dir = ::opendir("/dev");
    if (dir == nullptr) return found;

    const char* const prefixes[] = {"cu.usbmodem", "cu.usbserial", "cu.SLAB", "cu.wchusbserial"};

    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        for (const char* prefix : prefixes) {
            if (name.rfind(prefix, 0) == 0) {
                found.push_back("/dev/" + name);
                break;
            }
        }
    }
    ::closedir(dir);

    std::sort(found.begin(), found.end());
    return found;
}

bool SerialPort::openTcp(const std::string& host, int port, std::string* error) {
    close();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        setError(error, errnoText("socket"));
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        setError(error, "некорректный адрес " + host);
        return false;
    }

#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string text = errnoText("не удалось подключиться к " + host + ":" +
                                           std::to_string(port));
        ::close(fd);
        setError(error, text);
        return false;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    fd_ = fd;
    path_ = "tcp://" + host + ":" + std::to_string(port);
    setError(error, "");
    return true;
}

bool SerialPort::launchHelper(const std::string& executablePath, std::string* error) {
    if (executablePath.empty()) {
        setError(error, "путь к helper пуст");
        return false;
    }

    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr) != 0) {
        setError(error, errnoText("posix_spawnattr_init"));
        return false;
    }

    short flags = 0;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags = static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    posix_spawnattr_setflags(&attr, flags);

    char* argv[] = {const_cast<char*>(executablePath.c_str()), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, executablePath.c_str(), nullptr, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);

    if (rc != 0) {
        setError(error, "не удалось запустить " + executablePath + ": " + std::strerror(rc));
        return false;
    }

    setError(error, "");
    return true;
}

bool SerialPort::isLocalPortListening(const std::string& host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    /* Без SO_REUSEADDR: если helper уже слушает, bind вернёт EADDRINUSE. */
    const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int saved = errno;
    ::close(fd);
    return rc != 0 && saved == EADDRINUSE;
}

}  // namespace piano_led

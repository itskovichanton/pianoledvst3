/**
 * Unsandboxed USB-мост для GarageBand.
 *
 * AU крутится в AUHostingService без сети и без /dev/cu.*. Этот процесс:
 *   - держит USB открытым с запуска (чтобы C6 не ресетился на каждый connect);
 *   - принимает плагин по unix-сокету и TCP;
 *   - забирает кадры из /tmp/pianoled-bridge, если сокеты песочница закрыла.
 */

#include <arpa/inet.h>
#include <atomic>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "led_protocol.h"
#include "piano_led/config.h"
#include "piano_led/serial_port.h"

using piano_led::SerialPort;
using piano_led::kBridgeDropDir;
using piano_led::kBridgeTcpHost;
using piano_led::kBridgeTcpPort;
using piano_led::kBridgeUnixPath;

extern "C" void pianoled_run_app_loop(void);

namespace {

FILE* g_log = nullptr;
SerialPort g_usb;
std::mutex g_usbMu;
std::atomic<int> g_liveClients{0};
std::chrono::steady_clock::time_point g_lastForceOpen{};

void logLine(const char* fmt, ...) {
    if (g_log == nullptr) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void openLog() {
    const char* home = ::getenv("HOME");
    if (home == nullptr) return;
    const std::string dir = std::string(home) + "/Library/Application Support/PianoLED";
    ::mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/bridge.log";
    g_log = std::fopen(path.c_str(), "a");
}

bool setNonblock(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool writeAllFd(int fd, const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd waiter{fd, POLLOUT, 0};
            if (::poll(&waiter, 1, 200) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool writeAtomic(const std::string& path, const std::uint8_t* data, std::size_t size) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    if (!writeAllFd(fd, data, size)) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    ::close(fd);
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

bool usbFdHealthyLocked() {
    if (!g_usb.isOpen()) return false;
    pollfd p{};
    p.fd = g_usb.nativeFd();
    p.events = POLLIN;
    const int ready = ::poll(&p, 1, 0);
    if (ready < 0) return false;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    return true;
}

bool pingUsbLocked(int timeoutMs) {
    if (!g_usb.isOpen()) return false;
    std::uint8_t encoded[32];
    const std::size_t n =
        led_proto_encode(LED_FRAME_PING, nullptr, 0, encoded, sizeof(encoded));
    if (n == 0) return false;
    std::string error;
    if (!g_usb.writeAll(encoded, n, &error, 200)) {
        logLine("USB ping write: %s", error.c_str());
        g_usb.close();
        return false;
    }

    led_proto_decoder_t decoder;
    led_proto_decoder_init(&decoder);
    std::uint8_t buf[128];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto got = g_usb.readSome(buf, sizeof(buf));
        if (got < 0) {
            logLine("USB ping read error");
            g_usb.close();
            return false;
        }
        for (std::ptrdiff_t i = 0; i < got; ++i) {
            if (led_proto_decoder_push(&decoder, buf[static_cast<std::size_t>(i)]) != 0 &&
                decoder.type == LED_FRAME_PONG)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    logLine("USB ping timeout — дескриптор мёртвый, закрываю");
    g_usb.close();
    return false;
}

bool tryOpenUsbLocked() {
    if (g_usb.isOpen()) return true;
    const std::vector<std::string> candidates = SerialPort::listCandidates();
    for (const std::string& path : candidates) {
        std::string error;
        if (g_usb.open(path, &error)) {
            logLine("USB %s", path.c_str());
            return true;
        }
        logLine("USB %s — %s", path.c_str(), error.c_str());
    }
    return false;
}

bool ensureUsbLocked() {
    if (g_usb.isOpen() && pingUsbLocked(400)) return true;
    g_usb.close();

    const auto now = std::chrono::steady_clock::now();
    if (g_lastForceOpen.time_since_epoch().count() != 0 &&
        now - g_lastForceOpen < std::chrono::seconds(3)) {
        return false;
    }
    g_lastForceOpen = now;
    if (!tryOpenUsbLocked()) return false;
    /* CDC open ресетит C6. Не открывать снова, пока прошивка не ответит. */
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    return pingUsbLocked(2500);
}

bool splice(int a, int b) {
    std::uint8_t buf[1024];
    for (;;) {
        pollfd pf[2]{};
        pf[0].fd = a;
        pf[0].events = POLLIN;
        pf[1].fd = b;
        pf[1].events = POLLIN;

        const int ready = ::poll(pf, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        for (int i = 0; i < 2; ++i) {
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
            if ((pf[i].revents & POLLIN) == 0) continue;

            const ssize_t n = ::read(pf[i].fd, buf, sizeof(buf));
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return false;
            }
            if (!writeAllFd(pf[1 - i].fd, buf, static_cast<std::size_t>(n))) return false;
        }
    }
}

void serveClient(int client) {
    int usbFd = -1;
    {
        std::lock_guard<std::mutex> lock(g_usbMu);
        g_liveClients.fetch_add(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline && !ensureUsbLocked())
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (g_usb.isOpen()) usbFd = g_usb.nativeFd();
    }

    if (usbFd < 0) {
        logLine("клиент есть, USB нет");
        ::close(client);
        std::lock_guard<std::mutex> lock(g_usbMu);
        g_liveClients.fetch_sub(1);
        return;
    }

    logLine("splice client=%d usb=%d", client, usbFd);
    splice(client, usbFd);
    ::close(client);
    {
        std::lock_guard<std::mutex> lock(g_usbMu);
        g_liveClients.fetch_sub(1);
        if (g_usb.isOpen() && !pingUsbLocked(250))
            logLine("USB не отвечает после клиента");
    }
    logLine("клиент отключился");
}

int listenTcp() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(kBridgeTcpPort));
    if (::inet_pton(AF_INET, kBridgeTcpHost, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    setNonblock(fd);
    return fd;
}

int listenUnix() {
    ::unlink(kBridgeUnixPath);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kBridgeUnixPath, sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    ::chmod(kBridgeUnixPath, 0777);
    setNonblock(fd);
    return fd;
}

void acceptLoop(int listenFd) {
    for (;;) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            logLine("accept: %s", std::strerror(errno));
            return;
        }
#if defined(SO_NOSIGPIPE)
        const int one = 1;
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        setNonblock(client);
        std::thread(serveClient, client).detach();
    }
}

void onAccept(void* ctx) {
    acceptLoop(static_cast<int>(reinterpret_cast<intptr_t>(ctx)));
}

void attachAccept(int listenFd, const char* name) {
    if (listenFd < 0) {
        logLine("нет слушателя %s", name);
        return;
    }
    logLine("слушаю %s fd=%d", name, listenFd);
    const dispatch_source_t src =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(listenFd), 0,
                               dispatch_get_main_queue());
    dispatch_set_context(src, reinterpret_cast<void*>(static_cast<intptr_t>(listenFd)));
    dispatch_source_set_event_handler_f(src, onAccept);
    dispatch_resume(src);
}

void ensureDropDir() {
    ::mkdir(kBridgeDropDir, 0777);
    ::chmod(kBridgeDropDir, 0777);
}

void writeStatus() {
    ensureDropDir();
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_usbMu);
        if (g_usb.isOpen())
            text = "ok " + g_usb.path() + "\n";
        else
            text = "wait\n";
    }
    writeAtomic(std::string(kBridgeDropDir) + "/status",
                reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::vector<std::uint8_t> takeFile(const std::string& path) {
    const std::string locked = path + ".reading";
    ::unlink(locked.c_str());
    if (::rename(path.c_str(), locked.c_str()) != 0) return {};
    const int fd = ::open(locked.c_str(), O_RDONLY);
    if (fd < 0) {
        ::unlink(locked.c_str());
        return {};
    }
    std::vector<std::uint8_t> data(2048);
    const ssize_t n = ::read(fd, data.data(), data.size());
    ::close(fd);
    ::unlink(locked.c_str());
    if (n <= 0) return {};
    data.resize(static_cast<std::size_t>(n));
    return data;
}

void pollDropDir() {
    std::lock_guard<std::mutex> lock(g_usbMu);
    if (g_liveClients.load() > 0) return;
    if (!g_usb.isOpen()) tryOpenUsbLocked();
    if (!g_usb.isOpen()) return;

    const auto outgoing = takeFile(std::string(kBridgeDropDir) + "/to_esp");
    if (!outgoing.empty()) {
        std::string error;
        if (!g_usb.writeAll(outgoing.data(), outgoing.size(), &error, 200)) {
            logLine("drop write: %s", error.c_str());
            g_usb.close();
            return;
        }
    }

    std::uint8_t buf[1024];
    const auto n = g_usb.readSome(buf, sizeof(buf));
    if (n < 0) {
        logLine("USB read error, закрываю");
        g_usb.close();
        return;
    }
    if (n > 0) {
        writeAtomic(std::string(kBridgeDropDir) + "/from_esp", buf, static_cast<std::size_t>(n));
    }
}

void onTimer(void*) {
    static int ticks = 0;
    ++ticks;
    /* ~3 с: не долбить CDC, иначе C6 вечно ресетится и не отвечает на PING. */
    if (ticks % 375 == 0) {
        {
            std::lock_guard<std::mutex> lock(g_usbMu);
            if (g_liveClients.load() == 0)
                ensureUsbLocked();
        }
        writeStatus();
    }
    pollDropDir();
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    const mode_t oldMask = ::umask(0);
    openLog();
    logLine("---- start pid=%d ----", static_cast<int>(::getpid()));

    ensureDropDir();
    {
        std::lock_guard<std::mutex> lock(g_usbMu);
        tryOpenUsbLocked();
    }
    writeStatus();

    const int tcpFd = listenTcp();
    if (tcpFd < 0 && errno == EADDRINUSE) {
        logLine("уже запущен (порт занят)");
        return 0;
    }

    const int unixFd = listenUnix();
    ::umask(oldMask);

    if (tcpFd < 0)
        logLine("TCP %s:%d не слушаю: %s", kBridgeTcpHost, kBridgeTcpPort, std::strerror(errno));

    attachAccept(tcpFd, "tcp");
    attachAccept(unixFd, "unix");

    const dispatch_source_t timer =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 8ull * 1000000ull,
                              1ull * 1000000ull);
    dispatch_source_set_event_handler_f(timer, onTimer);
    dispatch_resume(timer);

    pianoled_run_app_loop();
    return 0;
}

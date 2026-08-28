/**
 * Unsandboxed USB↔TCP мост для GarageBand.
 *
 * AU в песочнице не видит /dev/cu.usbmodem*. Этот процесс открывает порт
 * сам и пробрасывает те же байты протокола на 127.0.0.1:17321.
 *
 * Второй экземпляр сразу выходит: порт уже занят — это нормально.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "piano_led/config.h"
#include "piano_led/serial_port.h"

using piano_led::SerialPort;
using piano_led::kBridgeTcpHost;
using piano_led::kBridgeTcpPort;

namespace {

bool setNonblock(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool writeAll(int fd, const std::uint8_t* data, std::size_t size) {
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

bool openUsb(SerialPort& usb) {
    const std::vector<std::string> candidates = SerialPort::listCandidates();
    for (const std::string& path : candidates) {
        std::string error;
        if (usb.open(path, &error)) return true;
    }
    return false;
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
            if (!writeAll(pf[1 - i].fd, buf, static_cast<std::size_t>(n))) return false;
        }
    }
}

int listenLocal() {
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
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

extern "C" void pianoled_run_app_loop(void);

static void serveForever(int listener) {
    for (;;) {
        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        const int client = ::accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }

#if defined(SO_NOSIGPIPE)
        const int one = 1;
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        setNonblock(client);

        SerialPort usb;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (openUsb(usb)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!usb.isOpen()) {
            std::cerr << "ledbridged: клиент есть, но USB-порт не открылся\n";
            ::close(client);
            continue;
        }

        std::cerr << "ledbridged: " << usb.path() << " ↔ плагин\n";
        splice(client, usb.nativeFd());
        ::close(client);
        usb.close();
    }

    ::close(listener);
}

int main() {
    ::signal(SIGPIPE, SIG_IGN);

    const int listener = listenLocal();
    if (listener < 0) {
        if (errno == EADDRINUSE) return 0;
        std::cerr << "ledbridged: не удалось слушать " << kBridgeTcpHost << ":" << kBridgeTcpPort
                  << ": " << std::strerror(errno) << "\n";
        return 1;
    }

    std::cerr << "ledbridged: " << kBridgeTcpHost << ":" << kBridgeTcpPort
              << " — жду плагин, USB открою при подключении\n";

    std::thread(serveForever, listener).detach();
    pianoled_run_app_loop();
    return 0;
}

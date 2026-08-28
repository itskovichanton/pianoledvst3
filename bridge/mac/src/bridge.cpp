#include "piano_led/bridge.h"
#include "piano_led/config.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace piano_led {
namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

const char* tickResultName(TickResult result) {
    switch (result) {
        case TickResult::unchanged:   return "unchanged";
        case TickResult::sent:        return "sent";
        case TickResult::notOpen:     return "notOpen";
        case TickResult::writeFailed: return "writeFailed";
    }
    return "<неизвестно>";
}

LedBridge::LedBridge(StripLayout layout) : builder_(layout) {
    led_proto_decoder_init(&decoder_);
    outBuffer_.resize(builder_.frameSize() + LED_PROTO_OVERHEAD);
}

void LedBridge::setLayout(StripLayout layout) {
    builder_ = FrameBuilder(layout);
    outBuffer_.assign(builder_.frameSize() + LED_PROTO_OVERHEAD, 0u);
    everSent_ = false;  // геометрия сменилась, прошлый кадр больше не показателен
}

bool LedBridge::open(const std::string& devicePath, std::string* error) {
    led_proto_decoder_init(&decoder_);
    everSent_ = false;
    havePong_ = false;
    logs_.clear();

    std::string localError;
    if (!port_.open(devicePath, &localError)) {
        lastError_ = localError;
        if (error != nullptr) *error = localError;
        return false;
    }
    return true;
}

void LedBridge::adoptPortFd(int fd, std::string label) {
    led_proto_decoder_init(&decoder_);
    everSent_ = false;
    havePong_ = false;
    logs_.clear();
    port_.adoptFd(fd, std::move(label));
}

bool LedBridge::openAuto(std::string* error, int replyTimeoutMs, bool tryTcp) {
    const std::vector<std::string> candidates = SerialPort::listCandidates();
    std::string report;

    for (const std::string& candidate : candidates) {
        std::string openError;
        if (!open(candidate, &openError)) {
            report += "  " + candidate + " — " + openError + "\n";
            continue;
        }

        /* Устройств на /dev/cu.* может быть несколько, и слать кадры пикселей
         * в чужой порт нехорошо. PING/PONG отвечает только наша прошивка. */
        if (probe(replyTimeoutMs)) return true;

        report += "  " + candidate + " — открылся, но не ответил на PING\n";
        port_.close();
    }

    if (tryTcp) {
        std::string tcpError;
        bool tcpOpen = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            led_proto_decoder_init(&decoder_);
            everSent_ = false;
            havePong_ = false;
            logs_.clear();
            if (port_.openTcp(kBridgeTcpHost, kBridgeTcpPort, &tcpError)) {
                tcpOpen = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (tcpOpen && probe(replyTimeoutMs)) return true;
        port_.close();
        if (!tcpError.empty())
            report += "  helper " + std::string(kBridgeTcpHost) + ":" +
                      std::to_string(kBridgeTcpPort) + " — " + tcpError + "\n";
        else
            report += "  helper — открылся, но не ответил на PING\n";
    }

    if (candidates.empty()) {
        lastError_ =
            "GarageBand не видит USB (песочница AU). Нажми «Подключить ленту» "
            "или один раз открой PianoLEDBridge.app "
            "(~/Library/Application Support/PianoLED/).";
    } else {
        lastError_ = "лента не найдена. Проверенные порты:\n" + report +
                     "Если плата подключена — убедись, что на неё залита прошивка из firmware/";
    }
    if (error != nullptr) *error = lastError_;
    return false;
}

bool LedBridge::sendFrame(std::uint8_t type, const std::uint8_t* payload, std::uint16_t length,
                          std::string* error) {
    const std::size_t needed = led_proto_encoded_size(length);
    if (outBuffer_.size() < needed) outBuffer_.resize(needed);

    const std::size_t encoded =
        led_proto_encode(type, payload, length, outBuffer_.data(), outBuffer_.size());
    if (encoded == 0) {
        lastError_ = "не удалось закодировать кадр типа " + std::to_string(type);
        if (error != nullptr) *error = lastError_;
        return false;
    }

    std::string writeError;
    if (!port_.writeAll(outBuffer_.data(), encoded, &writeError)) {
        lastError_ = writeError;
        if (error != nullptr) *error = writeError;
        return false;
    }
    return true;
}

TickResult LedBridge::tick(bool force) {
    const NoteBitmask::Snapshot current = notes_.snapshot();

    /* Пока руки неподвижны, по проводу не идёт ничего. Это не оптимизация ради
     * оптимизации: пустой канал означает, что любая активность на нём — это
     * реально сыгранная нота, и отладка становится тривиальной. */
    if (!force && everSent_ && current == lastSent_) return TickResult::unchanged;

    builder_.build(current, kNoteColor);

    if (!port_.isOpen()) {
        /* Кадр всё равно собрали — UI плагина покажет предпросмотр даже без железа. */
        lastSent_ = current;
        everSent_ = true;
        return TickResult::notOpen;
    }

    const std::vector<std::uint8_t>& frame = builder_.frame();
    if (!sendFrame(LED_FRAME_PIXELS, frame.data(), static_cast<std::uint16_t>(frame.size()),
                   nullptr)) {
        return TickResult::writeFailed;
    }

    lastSent_ = current;
    everSent_ = true;

    pump();  // заодно разбираем то, что прошивка успела прислать
    return TickResult::sent;
}

bool LedBridge::sendClear(std::string* error) {
    if (!port_.isOpen()) {
        lastError_ = "порт не открыт";
        if (error != nullptr) *error = lastError_;
        return false;
    }

    if (!sendFrame(LED_FRAME_CLEAR, nullptr, 0, error)) return false;

    notes_.allNotesOff();
    lastSent_ = notes_.snapshot();
    everSent_ = true;
    return true;
}

void LedBridge::pump() {
    if (!port_.isOpen()) return;

    for (;;) {
        const std::ptrdiff_t read = port_.readSome(readBuffer_, sizeof(readBuffer_));
        if (read <= 0) return;

        for (std::ptrdiff_t i = 0; i < read; ++i) {
            if (!led_proto_decoder_push(&decoder_, readBuffer_[static_cast<std::size_t>(i)])) {
                continue;
            }

            if (decoder_.type == LED_FRAME_PONG) {
                pendingPong_.assign(decoder_.payload, decoder_.payload + decoder_.length);
                havePong_ = true;
            } else if (decoder_.type == LED_FRAME_LOG) {
                logs_.emplace_back(reinterpret_cast<const char*>(decoder_.payload),
                                   decoder_.length);
                /* Прошивка может разговориться, а читать логи никто не обязан —
                 * держим только последние. */
                if (logs_.size() > 200) logs_.erase(logs_.begin(), logs_.begin() + 100);
            }
        }
    }
}

bool LedBridge::waitForFrame(std::uint8_t wantedType, std::vector<std::uint8_t>* payloadOut,
                             int timeoutMs) {
    if (wantedType != LED_FRAME_PONG) return false;  // ждём только PONG

    const std::int64_t deadline = nowMs() + timeoutMs;
    havePong_ = false;

    while (nowMs() < deadline) {
        pump();
        if (havePong_) {
            if (payloadOut != nullptr) *payloadOut = pendingPong_;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool LedBridge::probe(int timeoutMs) {
    if (!port_.isOpen()) {
        lastError_ = "порт не открыт";
        return false;
    }

    const std::int64_t deadline = nowMs() + timeoutMs;
    std::string pingError;
    while (nowMs() < deadline) {
        const int slice = static_cast<int>(std::min<std::int64_t>(300, deadline - nowMs()));
        if (slice <= 0) break;
        if (ping(nullptr, &pingError, slice)) return true;
        if (!port_.isOpen()) return false;
    }
    lastError_ = "нет ответа на PING за " + std::to_string(timeoutMs) + " мс";
    return false;
}

bool LedBridge::ping(int* ledCountOut, std::string* error, int timeoutMs) {
    if (!port_.isOpen()) {
        lastError_ = "порт не открыт";
        if (error != nullptr) *error = lastError_;
        return false;
    }

    if (!sendFrame(LED_FRAME_PING, nullptr, 0, error)) return false;

    std::vector<std::uint8_t> payload;
    if (!waitForFrame(LED_FRAME_PONG, &payload, timeoutMs)) {
        lastError_ = "нет ответа на PING за " + std::to_string(timeoutMs) + " мс";
        if (error != nullptr) *error = lastError_;
        return false;
    }

    if (payload.size() < LED_PROTO_PONG_PAYLOAD_SIZE) {
        lastError_ = "PONG слишком короткий: " + std::to_string(payload.size()) + " байт";
        if (error != nullptr) *error = lastError_;
        return false;
    }

    if (ledCountOut != nullptr) {
        *ledCountOut = static_cast<int>(payload[1]) | (static_cast<int>(payload[2]) << 8);
    }
    return true;
}

std::vector<std::string> LedBridge::takeLogs() {
    pump();
    std::vector<std::string> taken;
    taken.swap(logs_);
    return taken;
}

}  // namespace piano_led

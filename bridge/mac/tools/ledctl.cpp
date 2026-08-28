/**
 * ledctl — проверка ленты без Logic и без плагина.
 *
 * Смысл этой утилиты в том, чтобы разделить отладку надвое. Когда «не
 * работает», виноватых двое: железо с прошивкой или плагин. ledctl говорит с
 * лентой тем же кодом, что и плагин, но без JUCE, без хоста и без аудио-потока.
 * Если гамма здесь бежит — железная половина исправна, и искать надо в плагине.
 *
 *   ledctl list              перечислить порты
 *   ledctl ping [порт]       проверить связь, узнать длину ленты
 *   ledctl scale [порт]      прогнать хроматическую гамму
 *   ledctl chord [порт]      зажечь аккорд и подержать
 *   ledctl note <midi>...    зажечь конкретные ноты
 *   ledctl off [порт]        погасить ленту
 *
 * Общие флаги:
 *   --port PATH        порт явно, вместо автопоиска
 *   --tempo-ms N       длительность ноты в гамме (по умолчанию 120)
 *   --hold-ms N        сколько держать аккорд/ноты (по умолчанию 3000)
 *
 * Яркость не настраивается: она всегда 1% (см. config.h).
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "piano_led/bridge.h"

using namespace piano_led;

namespace {

struct Options {
    std::string command = "help";
    std::string port;

    int tempoMs = 120;
    int holdMs = 3000;
    std::vector<int> notes;
};

void printHelp() {
    std::cout <<
        R"(ledctl — проверка светодиодной ленты без Logic и плагина

Команды:
  list                 перечислить последовательные порты
  ping                 проверить связь с лентой, узнать её длину
  scale                прогнать хроматическую гамму по всей ленте
  chord                зажечь до-мажорное трезвучие и подержать
  note <midi> [...]    зажечь ноты по MIDI-номерам (60 = C4)
  off                  погасить ленту

Флаги:
  --port PATH          порт явно, вместо автопоиска
  --tempo-ms N         длительность ноты в гамме, мс (по умолчанию 120)
  --hold-ms N          сколько держать ноты/аккорд, мс (по умолчанию 3000)

Яркость всегда 1% и не настраивается.

Примеры:
  ./run_ledctl.sh ping
  ./run_ledctl.sh scale --tempo-ms 200
  ./run_ledctl.sh note 60 64 67
)";
}

Options parseArgs(int argc, char** argv) {
    Options options;
    if (argc < 2) return options;

    options.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--port" && i + 1 < argc) {
            options.port = argv[++i];
        } else if (arg == "--tempo-ms" && i + 1 < argc) {
            options.tempoMs = std::atoi(argv[++i]);
        } else if (arg == "--hold-ms" && i + 1 < argc) {
            options.holdMs = std::atoi(argv[++i]);
        } else if (!arg.empty() && arg[0] != '-') {
            options.notes.push_back(std::atoi(arg.c_str()));
        } else {
            std::cerr << "Неизвестный аргумент: " << arg << "\n";
        }
    }

    return options;
}

void sleepMs(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/** Печатает всё, что прошивка успела рассказать о себе. */
void drainLogs(LedBridge& bridge) {
    for (const std::string& line : bridge.takeLogs()) {
        std::cout << "  [плата] " << line << "\n";
    }
}

/** Открывает порт: явно указанный или найденный автопоиском. */
bool connect(LedBridge& bridge, const Options& options) {
    std::string error;

    if (!options.port.empty()) {
        if (!bridge.open(options.port, &error)) {
            std::cerr << "Не удалось открыть " << options.port << ": " << error << "\n";
            return false;
        }
        if (!bridge.probe()) {
            std::cerr << "Порт " << options.port << " открылся, но лента не отвечает: "
                      << bridge.lastError() << "\n"
                      << "Проверь, что на плату залита прошивка из firmware/.\n";
            return false;
        }
    } else {
        if (!bridge.openAuto(&error)) {
            std::cerr << error << "\n";
            return false;
        }
    }

    std::cout << "Лента: " << bridge.devicePath() << "\n";
    drainLogs(bridge);
    return true;
}

int commandList() {
    const std::vector<std::string> ports = SerialPort::listCandidates();

    if (ports.empty()) {
        std::cout << "Последовательных портов не найдено.\n"
                     "  • Проверь, что плата подключена\n"
                     "  • Кабель должен быть с данными, а не только для зарядки\n";
        return EXIT_FAILURE;
    }

    std::cout << "Найдено портов: " << ports.size() << "\n";
    for (const std::string& port : ports) std::cout << "  " << port << "\n";
    std::cout << "\nКакой из них наш — покажет ledctl ping\n";
    return EXIT_SUCCESS;
}

int commandPing(const Options& options) {
    LedBridge bridge;
    if (!connect(bridge, options)) return EXIT_FAILURE;

    int ledCount = 0;
    std::string error;
    if (!bridge.ping(&ledCount, &error)) {
        std::cerr << "PING без ответа: " << error << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Связь есть. Лента сообщает: " << ledCount << " светодиодов\n";

    const StripLayout& layout = bridge.layout();
    if (ledCount != layout.ledCount) {
        std::cout << "\nВНИМАНИЕ: прошивка собрана под " << ledCount
                  << " светодиодов, а хост настроен на " << layout.ledCount << ".\n"
                  << "Поправь LED_COUNT в firmware/main/main.c или StripLayout на стороне Mac —\n"
                  << "иначе часть ленты будет молча оставаться тёмной.\n";
    }

    drainLogs(bridge);
    return EXIT_SUCCESS;
}

int commandScale(const Options& options) {
    LedBridge bridge;
    if (!connect(bridge, options)) return EXIT_FAILURE;

    const StripLayout& layout = bridge.layout();
    const int lowest = layout.lowestNote;
    const int highest = layout.highestNote();

    std::cout << "Гамма: ноты " << lowest << ".." << highest << " (" << layout.keyCount()
              << " клавиш), по " << options.tempoMs << " мс, яркость 1%\n\n";

    for (int note = lowest; note <= highest; ++note) {
        bridge.noteOn(note);
        if (bridge.tick() == TickResult::writeFailed) {
            std::cerr << "Обрыв связи: " << bridge.lastError() << "\n";
            return EXIT_FAILURE;
        }

        std::cout << "\r  нота " << note << "  ток ~"
                  << static_cast<int>(bridge.estimatedCurrentMa()) << " мА   " << std::flush;

        sleepMs(options.tempoMs);

        bridge.noteOff(note);
        bridge.tick();
    }

    std::cout << "\n\nГамма пройдена. Если светодиоды бежали по ленте — железо исправно.\n";
    bridge.sendClear();
    drainLogs(bridge);
    return EXIT_SUCCESS;
}

int commandNotes(const Options& options, const std::vector<int>& notes, const char* title) {
    LedBridge bridge;
    if (!connect(bridge, options)) return EXIT_FAILURE;

    const StripLayout& layout = bridge.layout();
    for (const int note : notes) {
        if (!layout.covers(note)) {
            std::cout << "  нота " << note << " вне ленты (" << layout.lowestNote << ".."
                      << layout.highestNote() << ") — пропущена\n";
            continue;
        }
        bridge.noteOn(note);
    }

    if (bridge.tick() == TickResult::writeFailed) {
        std::cerr << "Обрыв связи: " << bridge.lastError() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << title << ": звучит нот " << bridge.activeNotes().count() << ", ток ~"
              << static_cast<int>(bridge.estimatedCurrentMa()) << " мА\n"
              << "Держу " << options.holdMs << " мс...\n";

    /* Прошивка гасит ленту, если хост молчит дольше двух секунд. Пока держим
     * картинку, надо изредка напоминать о себе — для этого force. */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.holdMs);
    while (std::chrono::steady_clock::now() < deadline) {
        sleepMs(500);
        bridge.tick(/*force=*/true);
    }

    bridge.sendClear();
    std::cout << "Погасил.\n";
    drainLogs(bridge);
    return EXIT_SUCCESS;
}

int commandOff(const Options& options) {
    LedBridge bridge;
    if (!connect(bridge, options)) return EXIT_FAILURE;

    std::string error;
    if (!bridge.sendClear(&error)) {
        std::cerr << "Не удалось погасить: " << error << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "Лента погашена.\n";
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parseArgs(argc, argv);

    if (options.command == "list") return commandList();
    if (options.command == "ping") return commandPing(options);
    if (options.command == "scale") return commandScale(options);
    if (options.command == "off") return commandOff(options);

    if (options.command == "chord") {
        return commandNotes(options, {60, 64, 67}, "До-мажорное трезвучие");
    }

    if (options.command == "note") {
        if (options.notes.empty()) {
            std::cerr << "Укажи хотя бы один MIDI-номер: ledctl note 60 64 67\n";
            return EXIT_FAILURE;
        }
        return commandNotes(options, options.notes, "Ноты");
    }

    printHelp();
    return options.command == "help" ? EXIT_SUCCESS : EXIT_FAILURE;
}

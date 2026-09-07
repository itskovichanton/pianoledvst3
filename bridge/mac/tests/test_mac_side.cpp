/**
 * Тесты Mac-стороны: битовая маска нот, сборка кадра, геометрия ленты.
 *
 * Главный тест здесь — test_realtime_path_does_not_allocate. Он подменяет
 * глобальный operator new и проверяет, что путь, по которому идёт аудио-поток,
 * не делает НИ ОДНОЙ аллокации. Это не формальность: аллокация в processBlock
 * может взять мьютекс кучи и задержать аудио-поток на непредсказуемое время —
 * в Logic это слышно как щелчок или дропаут.
 */

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "piano_led/bridge.h"
#include "piano_led/frame_builder.h"
#include "piano_led/midi_thru.h"
#include "piano_led/note_bitmask.h"
#include "piano_led/note_history.h"
#include "test_framework.h"

/* ══════════════════ счётчик аллокаций ══════════════════
 * Подменяем глобальные операторы на весь тестовый бинарник. Считаем только
 * вызовы; память по-прежнему берём у malloc, чтобы всё работало как обычно. */

namespace {
std::atomic<long> g_allocations{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace piano_led;
using namespace test_framework;

namespace {

/** Считает аллокации внутри переданного действия. */
template <typename Action>
long countAllocations(Action action) {
    g_allocations.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    action();
    g_counting.store(false, std::memory_order_relaxed);
    return g_allocations.load(std::memory_order_relaxed);
}

/** Индексы горящих светодиодов — компактное представление кадра. */
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

/* ══════════════════════ NoteBitmask ══════════════════════ */

void test_bitmask_basics() {
    begin_test("NoteBitmask — включение и выключение нот");

    NoteBitmask notes;
    check_eq(notes.snapshot().count(), 0, "изначально не звучит ничего");

    notes.noteOn(60);
    check(notes.snapshot().isOn(60), "нота 60 зажглась");
    check(!notes.snapshot().isOn(61), "соседняя нота не задета");
    check_eq(notes.snapshot().count(), 1, "звучит одна нота");

    notes.noteOn(61);
    notes.noteOn(72);
    check_eq(notes.snapshot().count(), 3, "звучат три ноты");

    notes.noteOff(61);
    check(!notes.snapshot().isOn(61), "нота 61 погасла");
    check(notes.snapshot().isOn(60) && notes.snapshot().isOn(72), "остальные не тронуты");

    notes.allNotesOff();
    check_eq(notes.snapshot().count(), 0, "allNotesOff погасил всё");
}

void test_bitmask_word_boundary() {
    begin_test("NoteBitmask — граница между двумя словами");

    /* Маска — два uint64. Ноты 63 и 64 лежат в разных словах: классическое
     * место для ошибки на единицу. */
    NoteBitmask notes;

    notes.noteOn(63);
    check(notes.snapshot().isOn(63), "нота 63 (последняя в младшем слове)");
    check(!notes.snapshot().isOn(64), "нота 64 не зажглась заодно");

    notes.noteOn(64);
    check(notes.snapshot().isOn(64), "нота 64 (первая в старшем слове)");

    notes.noteOff(63);
    check(!notes.snapshot().isOn(63), "63 погасла");
    check(notes.snapshot().isOn(64), "64 продолжает звучать");

    notes.noteOn(0);
    notes.noteOn(127);
    check(notes.snapshot().isOn(0), "самая нижняя нота 0");
    check(notes.snapshot().isOn(127), "самая верхняя нота 127");
}

void test_bitmask_ignores_out_of_range() {
    begin_test("NoteBitmask — ноты вне 0..127");

    NoteBitmask notes;
    notes.noteOn(-1);
    notes.noteOn(128);
    notes.noteOn(100000);
    notes.noteOff(-5);

    check_eq(notes.snapshot().count(), 0, "мусорные номера нот проигнорированы");
    check(!notes.snapshot().isOn(-1), "isOn(-1) == false, а не выход за массив");
    check(!notes.snapshot().isOn(999), "isOn(999) == false");
}

void test_bitmask_repeated_events() {
    begin_test("NoteBitmask — повторные события");

    NoteBitmask notes;

    /* Клавиатуры шлют повторный note on при ретригере, а хост — лишние note off. */
    notes.noteOn(60);
    notes.noteOn(60);
    check_eq(notes.snapshot().count(), 1, "двойной noteOn не задваивает ноту");

    notes.noteOff(60);
    notes.noteOff(60);
    check_eq(notes.snapshot().count(), 0, "двойной noteOff не ломает состояние");
}

/* ══════════════════════ REALTIME ══════════════════════ */

void test_allocation_detector_works() {
    begin_test("Детектор аллокаций — проверка самого детектора");

    /* Тест «ничего не выделилось» бесполезен, если счётчик сломан и всегда
     * показывает ноль. Убеждаемся, что он видит настоящую аллокацию. */
    const long detected = countAllocations([] {
        std::vector<int>* leaked = new std::vector<int>(100);
        delete leaked;
    });

    check(detected > 0, "детектор увидел заведомую аллокацию (" + std::to_string(detected) +
                            ") — значит нулям в тестах ниже можно верить");

    const long none = countAllocations([] {
        volatile int untouched = 0;
        (void)untouched;
    });
    check_eq(none, 0L, "на коде без аллокаций детектор молчит");
}

void test_realtime_path_does_not_allocate() {
    begin_test("АУДИО-ПОТОК — путь processBlock не аллоцирует");

    NoteBitmask notes;

    /* Прогреваем: если бы что-то выделялось лениво при первом вызове,
     * оно выделится здесь, до начала подсчёта. */
    notes.noteOn(60);
    notes.noteOff(60);

    const long allocations = countAllocations([&] {
        /* Ровно то, что делает processBlock на буфере, набитом MIDI-событиями. */
        for (int repeat = 0; repeat < 1000; ++repeat) {
            for (int note = 21; note <= 108; ++note) notes.noteOn(note);
            for (int note = 21; note <= 108; ++note) notes.noteOff(note);
            notes.allNotesOff();
            volatile auto snapshot = notes.snapshot();
            (void)snapshot;
        }
    });

    check_eq(allocations, 0L,
             "176 000 MIDI-событий не вызвали ни одной аллокации в аудио-потоке");
}

void test_bridge_note_calls_do_not_allocate() {
    begin_test("АУДИО-ПОТОК — LedBridge::noteOn/noteOff не аллоцируют");

    /* Плагин зовёт мост, а не маску напрямую — значит проверять надо мост. */
    LedBridge bridge(StripLayout{});
    bridge.noteOn(60);
    bridge.noteOff(60);

    const long allocations = countAllocations([&] {
        for (int repeat = 0; repeat < 1000; ++repeat) {
            for (int note = 36; note <= 83; ++note) bridge.noteOn(note);
            for (int note = 36; note <= 83; ++note) bridge.noteOff(note);
            bridge.allNotesOff();
        }
    });

    check_eq(allocations, 0L, "вызовы моста из аудио-потока не аллоцируют");
}

void test_note_history_push_does_not_allocate() {
    begin_test("АУДИО-ПОТОК — NoteHistory::push не аллоцирует");

    NoteHistory history;
    history.push(60);

    const long allocations = countAllocations([&] {
        for (int i = 0; i < 10000; ++i) history.push(36 + (i % 48));
    });
    check_eq(allocations, 0L, "история note-on не аллоцирует");
}

void test_frame_build_does_not_allocate() {
    begin_test("Поток таймера — сборка кадра не аллоцирует");

    /* Сборка идёт не в аудио-потоке, но буфер выделен в конструкторе,
     * и build() обязан только писать в него. */
    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;
    for (int note = 36; note <= 60; ++note) notes.noteOn(note);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));

    const long allocations = countAllocations([&] {
        for (int repeat = 0; repeat < 1000; ++repeat) {
            builder.build(notes.snapshot(), Rgb(3, 0, 0));
        }
    });

    check_eq(allocations, 0L, "1000 сборок кадра не вызвали аллокаций");
}

/* ══════════════════════ FrameBuilder ══════════════════════ */

void test_layout_math() {
    begin_test("StripLayout — геометрия установки");

    StripLayout layout;  // 144 диода, 3 на клавишу, от ноты 36
    check_eq(layout.keyCount(), 48, "144 диода / 3 = 48 клавиш");
    check_eq(layout.lowestNote, 36, "нижняя нота 36 (C2)");
    check_eq(layout.highestNote(), 83, "верхняя нота 83 (B5)");
    check(layout.isValid(), "конфигурация по умолчанию корректна");

    check(layout.covers(36) && layout.covers(83), "края диапазона покрыты");
    check(!layout.covers(35), "нота ниже ленты не покрыта");
    check(!layout.covers(84), "нота выше ленты не покрыта");

    StripLayout tooHigh;
    tooHigh.lowestNote = 100;  // 100 + 48 - 1 = 147 > 127
    check(!tooHigh.isValid(), "раскладка, вылезающая за MIDI 127, отвергнута");
}

void test_frame_note_to_leds() {
    begin_test("FrameBuilder — нота превращается в три светодиода");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;

    notes.noteOn(36);  // самая нижняя клавиша
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("0,1,2"), "нота 36 -> диоды 0,1,2");
    check_eq(int(builder.frame()[0]), 3, "красный канал равен заданной яркости");
    check_eq(int(builder.frame()[1]), 0, "зелёный канал нулевой");
    check_eq(int(builder.frame()[2]), 0, "синий канал нулевой");

    notes.allNotesOff();
    notes.noteOn(37);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("3,4,5"), "нота 37 -> диоды 3,4,5");

    notes.allNotesOff();
    notes.noteOn(83);  // самая верхняя клавиша
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("141,142,143"),
             "нота 83 -> последние три диода ленты");
}

void test_frame_chord() {
    begin_test("FrameBuilder — аккорд");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;

    notes.noteOn(60);  // C4
    notes.noteOn(64);  // E4
    notes.noteOn(67);  // G4
    builder.build(notes.snapshot(), Rgb(3, 0, 0));

    check_eq(litLeds(builder.frame()), std::string("72,73,74,84,85,86,93,94,95"),
             "до-мажорное трезвучие зажгло три группы по три диода");
}

void test_frame_ignores_notes_outside_strip() {
    begin_test("FrameBuilder — ноты за пределами ленты");

    /* Клавиатура на 88 клавиш шире метра ленты. Ноты снаружи надо молча
     * пропускать, а не писать за границу буфера. */
    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;

    notes.noteOn(21);   // A0 — ниже ленты
    notes.noteOn(108);  // C8 — выше ленты
    notes.noteOn(0);
    notes.noteOn(127);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string(""), "ни один диод не зажёгся");

    notes.noteOn(36);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("0,1,2"),
             "нота внутри диапазона работает, соседи снаружи не мешают");
    check_eq(builder.frame().size(), std::size_t{144 * 3}, "размер кадра не изменился");
}

void test_frame_reversed_strip() {
    begin_test("FrameBuilder — лента уложена справа налево");

    StripLayout layout;
    layout.reversed = true;
    FrameBuilder builder(layout);
    NoteBitmask notes;

    notes.noteOn(36);  // нижняя клавиша
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("141,142,143"),
             "нижняя нота ушла в конец ленты");

    notes.allNotesOff();
    notes.noteOn(83);  // верхняя клавиша
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("0,1,2"), "верхняя нота ушла в начало ленты");
}

void test_frame_clears_previous() {
    begin_test("FrameBuilder — кадр не тянет за собой прошлый");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;

    notes.noteOn(60);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check(!litLeds(builder.frame()).empty(), "нота зажглась");

    notes.noteOff(60);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string(""),
             "после снятия ноты кадр пуст, а не хранит остатки");
}

void test_frame_custom_geometry() {
    begin_test("FrameBuilder — другая геометрия");

    /* Например лента на 60 диодов по 2 на клавишу от ноты 48. */
    StripLayout layout;
    layout.ledCount = 60;
    layout.ledsPerKey = 2;
    layout.lowestNote = 48;

    FrameBuilder builder(layout);
    check_eq(builder.frameSize(), std::size_t{180}, "кадр 60 диодов = 180 байт");
    check_eq(layout.keyCount(), 30, "30 клавиш");

    NoteBitmask notes;
    notes.noteOn(49);
    builder.build(notes.snapshot(), Rgb(10, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("2,3"), "вторая клавиша -> диоды 2,3");
}

void test_variable_key_sizes_shift_neighbors() {
    begin_test("StripLayout — размер клавиши сдвигает следующие диоды");

    StripLayout layout;
    layout.makeSizesExplicit();
    layout.startLed = 0;
    layout.setKeySize(0, 3);
    layout.setKeySize(1, 2);
    layout.setKeySize(2, 3);

    check_eq(layout.ledStartForKey(0), 0, "первая клавиша с диода 0");
    check_eq(layout.ledStartForKey(1), 3, "вторая сразу после размера первой");
    check_eq(layout.ledStartForKey(2), 5, "третья сдвинулась на 2, а не на 3");

    layout.setKeySize(1, 4);
    check_eq(layout.ledStartForKey(2), 7, "увеличение размера сдвинуло хвост");

    FrameBuilder builder(layout);
    NoteBitmask notes;
    notes.noteOn(37);  // вторая клавиша, C#2
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("3,4,5,6"), "вторая клавиша — 4 диода");
}

void test_start_led_and_key_count() {
    begin_test("StripLayout — стартовый диод и число клавиш");

    StripLayout layout;
    layout.startLed = 10;
    layout.setMappedKeyCount(12);
    check_eq(layout.keyCount(), 12, "клавиш стало 12");
    check_eq(layout.highestNote(), 47, "12 клавиш от C2 заканчиваются на B2");
    check_eq(layout.ledStartForKey(0), 10, "первая клавиша начинается с диода 10");
}

void test_frame_single_led() {
    begin_test("FrameBuilder — один диод для бегущего теста");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    builder.lightLed(0, Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("0"), "первый диод ленты");

    builder.clear();
    builder.lightLed(143, Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("143"), "последний диод ленты");

    builder.lightLed(-1, Rgb(3, 0, 0));
    builder.lightLed(144, Rgb(3, 0, 0));
    check_eq(litLeds(builder.frame()), std::string("143"), "индекс вне ленты игнорируется");
}

void test_bridge_chase_overrides_notes() {
    begin_test("LedBridge — бегущий диод перекрывает ноты");

    LedBridge bridge(StripLayout{});
    bridge.noteOn(60);
    bridge.tick();
    check_eq(litLeds(bridge.lastFrame()), std::string("72,73,74"), "нота C4 зажглась");

    bridge.setChaseLed(5, true);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string("5"), "на ленте только бегущий диод");

    bridge.setChaseLed(-1, false);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string("72,73,74"), "после теста нота вернулась");
}

/* ══════════════════════ ток ══════════════════════ */

void test_current_estimate() {
    begin_test("Оценка тока при 1% яркости");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;

    /* Ради чего вся затея с 1%: даже вся лента целиком должна укладываться
     * в бюджет ноутбука. */
    for (int note = 36; note <= 83; ++note) notes.noteOn(note);
    builder.build(notes.snapshot(), Rgb(3, 0, 0));

    const double current = builder.estimatedCurrentMa();
    check_eq(litLeds(builder.frame()).empty(), false, "все 48 клавиш зажжены");
    check(current < 50.0, "вся лента на 1% берёт меньше 50 мА (получено " +
                              std::to_string(static_cast<int>(current)) + " мА)");
    check(current > 0.0, "оценка тока положительна");

    /* Для сравнения: полная яркость — то, чего допускать нельзя. */
    builder.build(notes.snapshot(), Rgb(255, 0, 0));
    check(builder.estimatedCurrentMa() > 2000.0,
          "на полной яркости та же картинка потребовала бы больше 2 А — "
          "поэтому лимит защищает прошивка, а не аккуратность хоста");

    notes.allNotesOff();
    builder.build(notes.snapshot(), Rgb(3, 0, 0));
    check_eq(builder.estimatedCurrentMa(), 0.0, "погашенная лента — 0 мА");
}

/* ══════════════════════ LedBridge без железа ══════════════════════ */

void test_led_style_default_is_two_percent_red() {
    begin_test("Стиль по умолчанию — красный 2%");

    const Rgb rgb = LedStyle{}.toRgb();
    check_eq(int(rgb.r), 5, "красный канал 5/255 — это 2%");
    check_eq(int(rgb.g), 0, "зелёный канал нулевой");
    check_eq(int(rgb.b), 0, "синий канал нулевой");
    check_eq(int(kNoteColor.r), 5, "kNoteColor совпадает с умолчанием");

    LedStyle green;
    green.hue = 120.0f;
    const Rgb g = green.toRgb();
    check_eq(int(g.r), 0, "зелёный оттенок: красный гаснет");
    check_eq(int(g.g), 5, "зелёный оттенок: зелёный канал 2%");
    check_eq(int(g.b), 0, "зелёный оттенок: синий гаснет");

    LedStyle white;
    white.saturation = 0.0f;
    white.brightnessPercent = 2;
    const Rgb w = white.toRgb();
    check_eq(int(w.r), 5, "белый: красный 2%");
    check_eq(int(w.g), 5, "белый: зелёный 2%");
    check_eq(int(w.b), 5, "белый: синий 2%");

    LedStyle dim;
    dim.brightnessPercent = 0.1f;
    const Rgb d = dim.toRgb();
    check_eq(int(d.r), 1, "0.1% — минимум диода, канал 1/255");
    check_eq(int(d.g), 0, "тусклый красный без зелёного");
    check_eq(int(d.b), 0, "тусклый красный без синего");
}

void test_bridge_uses_style_and_fill() {
    begin_test("LedBridge — стиль и заливка настроек сразу в кадре");

    LedBridge bridge(StripLayout{});
    bridge.noteOn(60);
    bridge.tick();
    check_eq(int(bridge.lastFrame()[72 * 3 + 0]), 5, "нота зажглась на 2%");

    LedStyle blue;
    blue.hue = 240.0f;
    blue.brightnessPercent = 2;
    bridge.setStyle(blue);
    bridge.tick();
    check_eq(int(bridge.lastFrame()[72 * 3 + 0]), 0, "после смены цвета красный гаснет");
    check_eq(int(bridge.lastFrame()[72 * 3 + 2]), 5, "синий канал 2%");

    bridge.setFillPreview(true);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string("67,68,69,70,71,72,73,74,75,76"),
             "превью — 10 диодов посередине, не вся лента");
    check_eq(int(bridge.lastFrame()[0]), 0, "край ленты погашен");
    check_eq(int(bridge.lastFrame()[67 * 3 + 2]), 5, "середина горит синим 2%");
    check_eq(int(bridge.lastFrame()[(144 * 3 - 1)]), 0, "другой край тоже погашен");

    bridge.setFillPreview(false);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string("72,73,74"),
             "после заливки снова горит только нота");
}

void test_note_history_window_and_recall() {
    begin_test("История нот — окно N и последние M");

    NoteHistory history;
    history.setCapacity(4);
    history.push(60);
    history.push(64);
    history.push(67);
    history.push(71);
    history.push(72);  // пятая; при N=4 нота 60 уже за окном

    check_eq(history.size(), 4, "окно N=4, пятая нота вытеснила первую из вида");
    const auto four = history.asSnapshot(4);
    check(four.isOn(64) && four.isOn(67) && four.isOn(71) && four.isOn(72),
          "последние 4 высоты на месте");
    check(!four.isOn(60), "первая нота уже за окном N");

    const auto two = history.asSnapshot(2);
    check(two.isOn(71) && two.isOn(72), "M=2 — две последние");
    check(!two.isOn(64) && !two.isOn(67), "более ранние не входят в M");
}

void test_note_history_last_chord_by_time() {
    begin_test("История — последний аккорд по окну времени");

    NoteHistory history;
    history.pushAt(60, 1000);
    history.pushAt(64, 1010);
    history.pushAt(67, 1020);
    history.pushAt(72, 2000);
    history.pushAt(76, 2015);

    const auto chord = history.lastChordSnapshot(50);
    check(chord.isOn(72) && chord.isOn(76), "последний аккорд — две ноты рядом");
    check(!chord.isOn(60) && !chord.isOn(64) && !chord.isOn(67),
          "предыдущий аккорд не попал: пауза больше окна");

    const auto wide = history.lastChordSnapshot(2000);
    check(wide.isOn(60) && wide.isOn(76), "большое окно склеивает всё в один аккорд");

    history.pushAt(48, 5000);
    const auto single = history.lastChordSnapshot(40);
    check(single.isOn(48) && single.count() == 1, "одиночная нота — аккорд из одной");
}

void test_bridge_history_preview_lights_notes() {
    begin_test("LedBridge — превью последних нот зажигает пачку");

    LedBridge bridge(StripLayout{});
    NoteBitmask::Snapshot chord;
    chord.setOn(60);
    chord.setOn(64);
    chord.setOn(67);
    bridge.setHistoryPreview(chord, true);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string("72,73,74,84,85,86,93,94,95"),
             "C-E-G зажглись как при игре аккорда");

    bridge.setHistoryPreview({}, false);
    bridge.tick(true);
    check_eq(litLeds(bridge.lastFrame()), std::string(""), "после превью лента погасла");
}

void test_brightness_is_user_controlled() {
    begin_test("Яркость задаётся стилем, по умолчанию 2%");

    LedBridge bridge(StripLayout{});
    bridge.noteOn(60);
    bridge.tick();

    const std::vector<std::uint8_t>& frame = bridge.lastFrame();
    check_eq(int(frame[72 * 3 + 0]), 5, "мост зажёг светодиод на 5/255");
    check_eq(int(frame[72 * 3 + 1]), 0, "зелёный не зажёгся");
    check_eq(int(frame[72 * 3 + 2]), 0, "синий не зажёгся");

    LedStyle brighter;
    brighter.brightnessPercent = 10;
    bridge.setStyle(brighter);
    bridge.tick();
    check_eq(int(bridge.lastFrame()[72 * 3 + 0]), 26, "10% — пик канала 26");
}

void test_brightness_stays_within_budget() {
    begin_test("Яркость — вся лента укладывается в бюджет питания");

    FrameBuilder builder(StripLayout{});
    NoteBitmask notes;
    for (int note = 36; note <= 83; ++note) notes.noteOn(note);
    builder.build(notes.snapshot(), kNoteColor);

    const double current = builder.estimatedCurrentMa();
    check(current < 150.0, "вся лента на 2% берёт " +
                               std::to_string(static_cast<int>(current)) +
                               " мА — в пределах бюджета 150 мА");
}

void test_bridge_without_port() {
    begin_test("LedBridge — работа без подключённой ленты");

    LedBridge bridge(StripLayout{});
    check(!bridge.isOpen(), "порт изначально закрыт");

    bridge.noteOn(60);
    const TickResult result = bridge.tick();
    check_eq(std::string(tickResultName(result)), std::string("notOpen"),
             "без порта tick() возвращает notOpen");
    check_eq(litLeds(bridge.lastFrame()), std::string("72,73,74"),
             "кадр всё равно собран — UI плагина покажет предпросмотр");
}

void test_bridge_change_detection() {
    begin_test("LedBridge — кадр уходит только при изменении");

    LedBridge bridge(StripLayout{});

    bridge.noteOn(60);
    bridge.tick();  // первый кадр

    check_eq(std::string(tickResultName(bridge.tick())), std::string("unchanged"),
             "ноты не менялись — второй tick ничего не делает");
    check_eq(std::string(tickResultName(bridge.tick())), std::string("unchanged"),
             "и третий тоже");

    bridge.noteOn(64);
    check_eq(std::string(tickResultName(bridge.tick())), std::string("notOpen"),
             "новая нота — кадр пересобран");

    check_eq(std::string(tickResultName(bridge.tick(/*force=*/true))), std::string("notOpen"),
             "force заставляет отправить кадр без изменений (keepalive)");
}

void test_midi_thru_notes_and_channel()
{
    begin_test("MIDI thru — ноты и канал");

    MidiThruConfig cfg;
    cfg.channel = 1;
    MidiPacket out;

    check(filterMidi(cfg, 0x90, 60, 100, out), "note-on проходит");
    check_eq(static_cast<int>(out.size), 3, "note-on — 3 байта");
    check_eq(static_cast<int>(out.bytes[0]), 0x90, "канал принудительно 1");
    check_eq(static_cast<int>(out.bytes[1]), 60, "номер ноты");
    check_eq(static_cast<int>(out.bytes[2]), 100, "velocity");

    check(filterMidi(cfg, 0x95, 64, 80, out), "note-on с другого канала");
    check_eq(static_cast<int>(out.bytes[0]), 0x90, "переписан на канал 1");

    cfg.channel = 0;
    check(filterMidi(cfg, 0x95, 64, 80, out), "Omni сохраняет канал");
    check_eq(static_cast<int>(out.bytes[0]), 0x95, "канал 6 как был");

    check(filterMidi(cfg, 0x80, 64, 0, out), "note-off проходит");
    check(filterMidi(cfg, 0x90, 64, 0, out), "note-on velocity 0 проходит");
}

void test_midi_thru_mapped_keys_and_cc()
{
    begin_test("MIDI thru — раскладка и CC");

    MidiThruConfig cfg;
    cfg.channel = 1;
    cfg.mappedKeysOnly = true;
    cfg.lowestNote = 36;
    cfg.highestNote = 83;
    MidiPacket out;

    check(filterMidi(cfg, 0x90, 36, 90, out), "первая нота раскладки проходит");
    check(filterMidi(cfg, 0x90, 83, 90, out), "последняя нота раскладки проходит");
    check(!filterMidi(cfg, 0x90, 35, 90, out), "ниже раскладки — отброшена");
    check(!filterMidi(cfg, 0x90, 84, 90, out), "выше раскладки — отброшена");

    cfg.mappedKeysOnly = false;
    check(filterMidi(cfg, 0x90, 21, 90, out), "без фильтра весь диапазон");

    check(filterMidi(cfg, 0xB0, 64, 127, out), "sustain по умолчанию");
    check(!filterMidi(cfg, 0xB0, 1, 64, out), "modulation по умолчанию выкл");
    check(!filterMidi(cfg, 0xB0, 7, 100, out), "volume не шлём");
    check(filterMidi(cfg, 0xB0, 123, 0, out), "All Notes Off всегда");
    check(filterMidi(cfg, 0xB0, 120, 0, out), "All Sound Off всегда");

    cfg.sendSustain = false;
    check(!filterMidi(cfg, 0xB0, 64, 0, out), "sustain можно выключить");

    cfg.sendModulation = true;
    check(filterMidi(cfg, 0xB0, 1, 40, out), "modulation по флагу");
}

void test_midi_thru_pitch_pc_clock_panic()
{
    begin_test("MIDI thru — pitch, PC, clock, panic");

    MidiThruConfig cfg;
    MidiPacket out;

    check(!filterMidi(cfg, 0xE0, 0, 64, out), "pitch bend по умолчанию выкл");
    cfg.sendPitchBend = true;
    check(filterMidi(cfg, 0xE5, 0, 64, out), "pitch bend по флагу");
    check_eq(static_cast<int>(out.bytes[0]), 0xE0, "pitch на канал 1");

    check(!filterMidi(cfg, 0xC0, 12, 0, out), "program change по умолчанию выкл");
    cfg.sendProgramChange = true;
    cfg.channel = 3;
    check(filterMidi(cfg, 0xC0, 12, 0, out), "program change по флагу");
    check_eq(static_cast<int>(out.size), 2, "PC — 2 байта");
    check_eq(static_cast<int>(out.bytes[0]), 0xC2, "PC на канал 3");
    check_eq(static_cast<int>(out.bytes[1]), 12, "номер программы");

    check(!filterMidi(cfg, 0xF8, 0, 0, out), "clock отброшен");
    check(!filterMidi(cfg, 0xF0, 0, 0, out), "SysEx отброшен");
    check(!filterMidi(cfg, 0xA0, 60, 40, out), "poly aftertouch отброшен");

    MidiPacket panic[40];
    const int n1 = panicPackets(1, panic, 40);
    check_eq(n1, 2, "panic на один канал — 2 сообщения");
    check_eq(static_cast<int>(panic[0].bytes[0]), 0xB0, "panic канал 1");
    check_eq(static_cast<int>(panic[0].bytes[1]), 123, "All Notes Off");
    check_eq(static_cast<int>(panic[1].bytes[1]), 120, "All Sound Off");

    const int nAll = panicPackets(0, panic, 40);
    check_eq(nAll, 32, "Omni panic — 16 каналов × 2");
}

}  // namespace

int main() {
    std::cout << "Тесты Mac-стороны: маска нот, сборка кадра, мост\n";

    test_bitmask_basics();
    test_bitmask_word_boundary();
    test_bitmask_ignores_out_of_range();
    test_bitmask_repeated_events();

    test_allocation_detector_works();
    test_realtime_path_does_not_allocate();
    test_bridge_note_calls_do_not_allocate();
    test_note_history_push_does_not_allocate();
    test_frame_build_does_not_allocate();

    test_layout_math();
    test_frame_note_to_leds();
    test_frame_chord();
    test_frame_ignores_notes_outside_strip();
    test_frame_reversed_strip();
    test_frame_clears_previous();
    test_frame_custom_geometry();
    test_variable_key_sizes_shift_neighbors();
    test_start_led_and_key_count();
    test_frame_single_led();
    test_bridge_chase_overrides_notes();

    test_current_estimate();

    test_led_style_default_is_two_percent_red();
    test_bridge_uses_style_and_fill();
    test_note_history_window_and_recall();
    test_note_history_last_chord_by_time();
    test_bridge_history_preview_lights_notes();
    test_brightness_is_user_controlled();
    test_brightness_stays_within_budget();

    test_bridge_without_port();
    test_bridge_change_detection();

    test_midi_thru_notes_and_channel();
    test_midi_thru_mapped_keys_and_cc();
    test_midi_thru_pitch_pc_clock_panic();

    return finish();
}

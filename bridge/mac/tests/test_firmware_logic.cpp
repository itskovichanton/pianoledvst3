/**
 * Тесты логики прошивки, прогоняемые на обычной машине.
 *
 * ESP-IDF здесь нет, собрать прошивку целиком нельзя. Но самое важное в ней —
 * защита по току и разбор кадра — намеренно вынесено в файлы без единой
 * зависимости от IDF (led_guard.c, led_protocol.c). Ровно те же .c-файлы,
 * что уедут на плату, компилируются и проверяются здесь.
 *
 * Защита по току — это то, что стоит между ошибкой в коде и питанием ноутбука,
 * поэтому проверяется подробно: граница, превышение, абсурдные значения,
 * сохранение пропорций цвета.
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "led_guard.h"
#include "led_protocol.h"
}

#include "test_framework.h"

using namespace test_framework;

namespace {

constexpr int kLedCount = 144;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kLedCount) * 3u;

/** Кадр, где заданное число светодиодов горит одним цветом. */
std::vector<std::uint8_t> makeFrame(int litLeds, std::uint8_t r, std::uint8_t g,
                                    std::uint8_t b) {
    std::vector<std::uint8_t> frame(kFrameBytes, 0u);
    for (int led = 0; led < litLeds && led < kLedCount; ++led) {
        frame[static_cast<std::size_t>(led) * 3 + 0] = r;
        frame[static_cast<std::size_t>(led) * 3 + 1] = g;
        frame[static_cast<std::size_t>(led) * 3 + 2] = b;
    }
    return frame;
}

double toMa(std::uint32_t microamps) { return static_cast<double>(microamps) / 1000.0; }

/* ══════════════════════ оценка тока ══════════════════════ */

void test_current_estimate() {
    begin_test("Защита по току — оценка");

    std::vector<std::uint8_t> dark(kFrameBytes, 0u);
    check_eq(led_guard_estimate_ua(dark.data(), dark.size()), 0u, "погашенный кадр — 0 мкА");

    /* Один светодиод, красный на полную: ровно ток одного канала. */
    std::vector<std::uint8_t> one = makeFrame(1, 255, 0, 0);
    check_eq(led_guard_estimate_ua(one.data(), one.size()), LED_GUARD_CHANNEL_UA,
             "один светодиод на полном красном — 20 мА");

    /* Белый на полную — три канала. */
    std::vector<std::uint8_t> white = makeFrame(1, 255, 255, 255);
    check_eq(led_guard_estimate_ua(white.data(), white.size()), 3u * LED_GUARD_CHANNEL_UA,
             "один белый светодиод — 60 мА");

    /* Половина яркости — примерно половина тока. */
    std::vector<std::uint8_t> half = makeFrame(1, 128, 0, 0);
    const double halfMa = toMa(led_guard_estimate_ua(half.data(), half.size()));
    check(halfMa > 9.5 && halfMa < 10.5,
          "светодиод на половине яркости — около 10 мА (получено " +
              std::to_string(halfMa) + ")");
}

void test_channel_cap_at_one_percent() {
    begin_test("Защита — канал не выше 1% независимо от хоста");

    std::vector<std::uint8_t> one = makeFrame(1, 255, 128, 40);
    led_guard_clamp_channels(one.data(), one.size(), LED_GUARD_MAX_CHANNEL);
    check_eq(int(one[0]), int(LED_GUARD_MAX_CHANNEL), "красный обрезан до 3");
    check_eq(int(one[1]), int(LED_GUARD_MAX_CHANNEL), "зелёный обрезан до 3");
    check_eq(int(one[2]), int(LED_GUARD_MAX_CHANNEL), "синий обрезан до 3");
    check_eq(int(one[3]), 0, "соседний светодиод не зажёгся");

    std::vector<std::uint8_t> dim = makeFrame(4, 3, 0, 0);
    led_guard_clamp_channels(dim.data(), dim.size(), LED_GUARD_MAX_CHANNEL);
    check_eq(int(dim[0]), 3, "рабочий кадр плагина не меняется");

    led_guard_clamp_channels(nullptr, 100, LED_GUARD_MAX_CHANNEL);
}

void test_one_percent_is_safe() {
    begin_test("Защита по току — рабочий режим 1%");

    /* Ровно тот кадр, который будет слать плагин: 1% красного.
     * Даже если разом зажечь всю ленту, лимит не должен быть даже близко. */
    std::vector<std::uint8_t> full = makeFrame(kLedCount, 3, 0, 0);
    const double ma = toMa(led_guard_estimate_ua(full.data(), full.size()));

    check(ma < 50.0, "вся лента на 1% берёт " + std::to_string(static_cast<int>(ma)) +
                         " мА — меньше 50");

    const int clamped = led_guard_apply(full.data(), full.size(), 150u);
    check_eq(clamped, 0, "притушивать не пришлось");
    check_eq(int(full[0]), 3, "яркость осталась ровно такой, какую задал хост");

    /* Десять пальцев по три светодиода — типичный аккорд. */
    std::vector<std::uint8_t> chord = makeFrame(30, 3, 0, 0);
    const double chordMa = toMa(led_guard_estimate_ua(chord.data(), chord.size()));
    check(chordMa < 10.0, "аккорд из десяти клавиш — около " +
                              std::to_string(chordMa) + " мА");
}

/* ══════════════════════ притушивание ══════════════════════ */

void test_clamps_over_budget() {
    begin_test("Защита по току — кадр сверх бюджета притушивается");

    /* Хост по ошибке прислал полную яркость на всю ленту: 144 * 20 мА = 2.88 А.
     * Именно от такого прошивка и обязана защитить. */
    std::vector<std::uint8_t> frame = makeFrame(kLedCount, 255, 0, 0);
    const double before = toMa(led_guard_estimate_ua(frame.data(), frame.size()));
    check(before > 2000.0, "исходный кадр требовал бы " +
                               std::to_string(static_cast<int>(before)) + " мА");

    const int clamped = led_guard_apply(frame.data(), frame.size(), 150u);
    check_eq(clamped, 1, "кадр помечен как притушенный");

    const double after = toMa(led_guard_estimate_ua(frame.data(), frame.size()));
    check(after <= 150.0, "после правки ток " + std::to_string(after) + " мА не выше бюджета");
    check(after > 100.0, "и при этом бюджет использован, а не выброшен впустую");

    check(frame[0] > 0, "лента не погашена полностью — картинка осталась видимой");
    check(frame[0] < 255, "но яркость снижена");
}

void test_clamp_preserves_color_ratio() {
    begin_test("Защита по току — пропорции цвета сохраняются");

    /* Притушивание должно быть общим множителем: оранжевый обязан остаться
     * оранжевым, а не уехать в красный из-за обрезки каналов по отдельности. */
    std::vector<std::uint8_t> frame = makeFrame(kLedCount, 200, 100, 0);
    led_guard_apply(frame.data(), frame.size(), 150u);

    const double red = frame[0];
    const double green = frame[1];
    check(green > 0.0, "зелёный канал не обнулился");

    const double ratio = red / green;
    check(ratio > 1.7 && ratio < 2.3,
          "отношение красного к зелёному осталось около 2 (получено " +
              std::to_string(ratio) + ")");
    check_eq(int(frame[2]), 0, "синий как был нулём, так и остался");
}

void test_clamp_boundary() {
    begin_test("Защита по току — граница бюджета");

    /* Кадр ровно по бюджету трогать нельзя. 150 мА при 20 мА на канал —
     * это 7.5 светодиода на полном красном; берём 7, чтобы быть под границей. */
    std::vector<std::uint8_t> under = makeFrame(7, 255, 0, 0);
    const double underMa = toMa(led_guard_estimate_ua(under.data(), under.size()));
    check(underMa <= 150.0, "кадр на " + std::to_string(underMa) + " мА укладывается в бюджет");
    check_eq(led_guard_apply(under.data(), under.size(), 150u), 0, "не притушен");
    check_eq(int(under[0]), 255, "яркость не тронута");

    std::vector<std::uint8_t> over = makeFrame(9, 255, 0, 0);
    check_eq(led_guard_apply(over.data(), over.size(), 150u), 1, "кадр на 180 мА притушен");
    check(toMa(led_guard_estimate_ua(over.data(), over.size())) <= 150.0,
          "после правки уложился в бюджет");
}

void test_clamp_edge_cases() {
    begin_test("Защита по току — крайние случаи");

    std::vector<std::uint8_t> frame = makeFrame(kLedCount, 255, 255, 255);
    check_eq(led_guard_apply(frame.data(), frame.size(), 0u), 1, "нулевой бюджет -> притушен");
    bool allDark = true;
    for (const std::uint8_t value : frame) {
        if (value != 0) allDark = false;
    }
    check(allDark, "при нулевом бюджете лента гаснет полностью");

    std::vector<std::uint8_t> dark(kFrameBytes, 0u);
    check_eq(led_guard_apply(dark.data(), dark.size(), 150u), 0,
             "погашенный кадр не требует правки");

    check_eq(led_guard_apply(nullptr, 100, 150u), 0, "нулевой указатель не роняет");
    check_eq(led_guard_apply(dark.data(), 0, 150u), 0, "пустой кадр не роняет");
    check_eq(led_guard_estimate_ua(nullptr, 100), 0u, "оценка нулевого указателя — 0");

    /* Большой бюджет — правка не нужна даже на полной яркости. */
    std::vector<std::uint8_t> bright = makeFrame(kLedCount, 255, 255, 255);
    check_eq(led_guard_apply(bright.data(), bright.size(), 100000u), 0,
             "при огромном бюджете кадр проходит как есть");
    check_eq(int(bright[0]), 255, "яркость не тронута");
}

void test_clamp_is_idempotent() {
    begin_test("Защита по току — повторное применение ничего не портит");

    std::vector<std::uint8_t> frame = makeFrame(kLedCount, 255, 0, 0);
    led_guard_apply(frame.data(), frame.size(), 150u);
    const std::vector<std::uint8_t> afterFirst = frame;

    const int second = led_guard_apply(frame.data(), frame.size(), 150u);
    check_eq(second, 0, "второй проход не находит превышения");
    check(frame == afterFirst, "кадр не изменился при повторном применении");
}

/* ══════════════════════ разбор кадра прошивкой ══════════════════════ */

void test_firmware_accepts_host_frame() {
    begin_test("Прошивка — принимает кадр от Mac");

    /* Собираем кадр ровно так, как это делает LedBridge, и скармливаем
     * декодеру ровно так, как это делает главный цикл прошивки. */
    const std::vector<std::uint8_t> pixels = makeFrame(1, 3, 0, 0);

    std::vector<std::uint8_t> wire(led_proto_encoded_size(
        static_cast<std::uint16_t>(pixels.size())));
    const std::size_t size = led_proto_encode(LED_FRAME_PIXELS, pixels.data(),
                                              static_cast<std::uint16_t>(pixels.size()),
                                              wire.data(), wire.size());
    check(size > 0, "кадр закодирован");

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);

    int received = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (led_proto_decoder_push(&decoder, wire[i])) ++received;
    }

    check_eq(received, 1, "прошивка распознала один кадр");
    check_eq(int(decoder.type), int(LED_FRAME_PIXELS), "тип PIXELS");
    check_eq(decoder.length, std::uint16_t{kFrameBytes}, "длина 432 байта");
    check_eq(decoder.length % 3u, 0u, "длина кратна 3 — проверка прошивки пройдена");
    check_eq(int(decoder.payload[0]), 3, "яркость 1% доехала");
}

void test_firmware_rejects_misaligned_length() {
    begin_test("Прошивка — отвергает длину, не кратную 3");

    /* Такой кадр технически валиден по протоколу, но пикселями быть не может.
     * Прошивка обязана его отбросить, а не читать за границу буфера. */
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4};  // 4 не делится на 3

    std::vector<std::uint8_t> wire(led_proto_encoded_size(4));
    led_proto_encode(LED_FRAME_PIXELS, payload.data(), 4, wire.data(), wire.size());

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);
    for (const std::uint8_t byte : wire) led_proto_decoder_push(&decoder, byte);

    check_eq(decoder.length % 3u, 1u, "длина действительно не кратна 3");
    check(decoder.length % 3u != 0u, "именно это условие прошивка и проверяет перед memcpy");
}

void test_firmware_handles_short_frame() {
    begin_test("Прошивка — кадр от ленты другой длины");

    /* Хост настроен на 60 светодиодов, прошивка собрана под 144. Прошивка
     * берёт что пришло и гасит остаток — рассинхрон настроек виден глазом
     * (хвост ленты тёмный), но за границу буфера никто не пишет. */
    const std::size_t shortBytes = 60u * 3u;
    std::vector<std::uint8_t> pixels(shortBytes, 5u);

    std::vector<std::uint8_t> strip(kFrameBytes, 0u);
    std::size_t usable = pixels.size();
    if (usable > strip.size()) usable = strip.size();

    std::copy(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(usable),
              strip.begin());
    std::fill(strip.begin() + static_cast<std::ptrdiff_t>(usable), strip.end(),
              std::uint8_t{0});

    check_eq(int(strip[0]), 5, "начало ленты получило данные хоста");
    check_eq(int(strip[shortBytes - 1]), 5, "и до конца присланного куска");
    check_eq(int(strip[shortBytes]), 0, "остаток ленты погашен");
    check_eq(strip.size(), kFrameBytes, "буфер остался своего размера");
}

void test_firmware_ignores_unknown_frame_type() {
    begin_test("Прошивка — незнакомый тип кадра");

    /* Хост новее прошивки и прислал тип, о котором она не знает.
     * Декодер обязан кадр разобрать, а обработчик — молча пропустить,
     * не сбиваясь с потока. */
    const std::vector<std::uint8_t> payload = {1, 2, 3};
    std::vector<std::uint8_t> wire(led_proto_encoded_size(3));
    led_proto_encode(0x7F, payload.data(), 3, wire.data(), wire.size());

    const std::vector<std::uint8_t> pixels = makeFrame(1, 3, 0, 0);
    std::vector<std::uint8_t> next(led_proto_encoded_size(
        static_cast<std::uint16_t>(pixels.size())));
    led_proto_encode(LED_FRAME_PIXELS, pixels.data(),
                     static_cast<std::uint16_t>(pixels.size()), next.data(), next.size());

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);

    std::vector<int> types;
    for (const std::uint8_t byte : wire) {
        if (led_proto_decoder_push(&decoder, byte)) types.push_back(decoder.type);
    }
    for (const std::uint8_t byte : next) {
        if (led_proto_decoder_push(&decoder, byte)) types.push_back(decoder.type);
    }

    check_eq(types.size(), std::size_t{2}, "распознаны оба кадра");
    if (types.size() == 2) {
        check_eq(types[0], 0x7F, "первый — незнакомого типа");
        check_eq(types[1], int(LED_FRAME_PIXELS), "второй разобран нормально");
    }
}

}  // namespace

int main() {
    std::cout << "Тесты логики прошивки (те же .c, что уедут на плату)\n";

    test_current_estimate();
    test_channel_cap_at_one_percent();
    test_one_percent_is_safe();
    test_clamps_over_budget();
    test_clamp_preserves_color_ratio();
    test_clamp_boundary();
    test_clamp_edge_cases();
    test_clamp_is_idempotent();

    test_firmware_accepts_host_frame();
    test_firmware_rejects_misaligned_length();
    test_firmware_handles_short_frame();
    test_firmware_ignores_unknown_frame_type();

    return finish();
}

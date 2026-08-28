/**
 * Тесты протокола кадра.
 *
 * Этот файл проверяет ОБЕ стороны провода: кодировщик, которым пользуется Mac,
 * и декодер, который стоит в прошивке. Оба — один и тот же led_protocol.c, так
 * что зелёные тесты здесь означают, что стороны договорятся между собой.
 *
 * Отдельное внимание — ресинхронизации. Реальный поток начинается с мусора:
 * ROM-загрузчик C6 при включении что-то печатает, плагин может подключиться к
 * порту в середине кадра, плата может перезагрузиться. Декодер обязан из любого
 * такого состояния вернуться к разбору кадров.
 */

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "led_protocol.h"
#include "test_framework.h"

using namespace test_framework;

namespace {

std::vector<std::uint8_t> encode(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out(led_proto_encoded_size(
        static_cast<std::uint16_t>(payload.size())));
    const std::size_t written = led_proto_encode(
        type, payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint16_t>(payload.size()), out.data(), out.size());
    out.resize(written);
    return out;
}

/** Скармливает байты декодеру и собирает все распознанные кадры. */
struct DecodedFrame {
    std::uint8_t type;
    std::vector<std::uint8_t> payload;
};

std::vector<DecodedFrame> decodeAll(const std::vector<std::uint8_t>& bytes,
                                    led_proto_decoder_t* decoder) {
    std::vector<DecodedFrame> frames;
    for (const std::uint8_t byte : bytes) {
        if (led_proto_decoder_push(decoder, byte)) {
            frames.push_back({decoder->type,
                              std::vector<std::uint8_t>(decoder->payload,
                                                        decoder->payload + decoder->length)});
        }
    }
    return frames;
}

std::vector<DecodedFrame> decodeAll(const std::vector<std::uint8_t>& bytes) {
    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);
    return decodeAll(bytes, &decoder);
}

std::vector<std::uint8_t> makePixels(int ledCount, std::uint8_t value) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(ledCount) * 3u, value);
}

/* ══════════════════════ кодирование ══════════════════════ */

void test_encode_layout() {
    begin_test("Кодировщик — структура кадра");

    const std::vector<std::uint8_t> payload = {0x01, 0x02, 0x03};
    const std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, payload);

    check_eq(frame.size(), std::size_t{9}, "3 байта payload -> 9 байт кадра (5 заголовок + 1 xor)");
    check_eq(int(frame[0]), 0xAA, "байт 0 — магия 0xAA");
    check_eq(int(frame[1]), 0x55, "байт 1 — магия 0x55");
    check_eq(int(frame[2]), int(LED_FRAME_PIXELS), "байт 2 — тип кадра");
    check_eq(int(frame[3]), 3, "байт 3 — младший байт длины");
    check_eq(int(frame[4]), 0, "байт 4 — старший байт длины");
    check_eq(int(frame[8]), 0x01 ^ 0x02 ^ 0x03, "последний байт — xor payload");

    const std::vector<std::uint8_t> empty = encode(LED_FRAME_PING, {});
    check_eq(empty.size(), std::size_t{6}, "кадр без payload — 6 байт");
    check_eq(int(empty[5]), 0, "xor пустого payload == 0");
}

void test_encode_length_field() {
    begin_test("Кодировщик — двухбайтовая длина");

    /* 432 байта = 0x01B0: проверяем, что старший байт не потерялся.
     * Это ровно размер нашего кадра на 144 светодиода. */
    const std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, makePixels(144, 7));

    check_eq(frame.size(), std::size_t{438}, "144 светодиода -> 438 байт на проводе");
    check_eq(int(frame[3]), 0xB0, "младший байт длины 432");
    check_eq(int(frame[4]), 0x01, "старший байт длины 432");
}

void test_encode_rejects_bad_args() {
    begin_test("Кодировщик — отказы");

    std::uint8_t small[4] = {};
    const std::uint8_t payload[3] = {1, 2, 3};

    check_eq(led_proto_encode(LED_FRAME_PIXELS, payload, 3, small, sizeof(small)), std::size_t{0},
             "не хватает места в буфере -> 0");
    check_eq(led_proto_encode(LED_FRAME_PIXELS, payload, 3, nullptr, 100), std::size_t{0},
             "нулевой выходной буфер -> 0");
    check_eq(led_proto_encode(LED_FRAME_PIXELS, nullptr, 3, small, sizeof(small)), std::size_t{0},
             "длина без данных -> 0");

    std::vector<std::uint8_t> big(LED_PROTO_MAX_PAYLOAD + 100);
    std::vector<std::uint8_t> out(LED_PROTO_MAX_PAYLOAD + 200);
    check_eq(led_proto_encode(LED_FRAME_PIXELS, big.data(),
                              static_cast<std::uint16_t>(LED_PROTO_MAX_PAYLOAD + 1), out.data(),
                              out.size()),
             std::size_t{0}, "payload больше потолка -> 0");
}

/* ══════════════════════ круговой рейс ══════════════════════ */

void test_roundtrip() {
    begin_test("Круговой рейс — кодировщик Mac -> декодер прошивки");

    const std::vector<std::uint8_t> payload = makePixels(144, 3);
    const std::vector<DecodedFrame> frames = decodeAll(encode(LED_FRAME_PIXELS, payload));

    check_eq(frames.size(), std::size_t{1}, "распознан ровно один кадр");
    if (!frames.empty()) {
        check_eq(int(frames[0].type), int(LED_FRAME_PIXELS), "тип сохранился");
        check_eq(frames[0].payload.size(), payload.size(), "длина payload сохранилась");
        check(frames[0].payload == payload, "payload побайтово совпал");
    }
}

void test_roundtrip_all_types() {
    begin_test("Круговой рейс — все типы кадров");

    struct Case {
        std::uint8_t type;
        const char* name;
        std::vector<std::uint8_t> payload;
    };

    const std::vector<Case> cases = {
        {LED_FRAME_PIXELS, "PIXELS", makePixels(4, 200)},
        {LED_FRAME_PING, "PING", {}},
        {LED_FRAME_PONG, "PONG", {LED_PROTO_VERSION, 0x90, 0x00}},
        {LED_FRAME_LOG, "LOG", {'h', 'i'}},
        {LED_FRAME_CLEAR, "CLEAR", {}},
    };

    for (const Case& item : cases) {
        const std::vector<DecodedFrame> frames = decodeAll(encode(item.type, item.payload));
        check_eq(frames.size(), std::size_t{1}, std::string(item.name) + ": один кадр");
        if (!frames.empty()) {
            check_eq(int(frames[0].type), int(item.type),
                     std::string(item.name) + ": тип совпал");
            check(frames[0].payload == item.payload,
                  std::string(item.name) + ": payload совпал");
        }
    }
}

void test_back_to_back_frames() {
    begin_test("Поток — несколько кадров подряд без пауз");

    std::vector<std::uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        const std::vector<std::uint8_t> frame =
            encode(LED_FRAME_PIXELS, makePixels(2, static_cast<std::uint8_t>(i + 1)));
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    const std::vector<DecodedFrame> frames = decodeAll(stream);
    check_eq(frames.size(), std::size_t{5}, "распознаны все 5 кадров");
    for (std::size_t i = 0; i < frames.size(); ++i) {
        check_eq(int(frames[i].payload[0]), int(i + 1),
                 "кадр " + std::to_string(i) + " не перепутан с соседями");
    }
}

/* ══════════════════════ ресинхронизация ══════════════════════ */

void test_recovers_after_garbage() {
    begin_test("Ресинхронизация — мусор перед кадром");

    /* Так выглядит реальность: плата только что включилась и ROM-загрузчик
     * успел напечатать текст в тот же USB-порт. */
    std::vector<std::uint8_t> stream;
    const std::string boot = "ESP-ROM:esp32c6-20220919\r\nBuild:Sep 19 2022\r\n";
    stream.assign(boot.begin(), boot.end());

    const std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, makePixels(2, 42));
    stream.insert(stream.end(), frame.begin(), frame.end());

    const std::vector<DecodedFrame> frames = decodeAll(stream);
    check_eq(frames.size(), std::size_t{1}, "кадр после текстового мусора найден");
    if (!frames.empty()) check_eq(int(frames[0].payload[0]), 42, "payload не пострадал");
}

void test_recovers_after_truncated_frame() {
    begin_test("Ресинхронизация — оборванный кадр");

    /* Подключились к порту в середине кадра: первые байты — хвост чужого
     * сообщения, полноценного заголовка у него уже нет. */
    const std::vector<std::uint8_t> whole = encode(LED_FRAME_PIXELS, makePixels(8, 99));
    std::vector<std::uint8_t> stream(whole.begin() + 10, whole.end());  // обрезали начало

    const std::vector<std::uint8_t> good = encode(LED_FRAME_PIXELS, makePixels(2, 55));
    stream.insert(stream.end(), good.begin(), good.end());

    const std::vector<DecodedFrame> frames = decodeAll(stream);
    check_eq(frames.size(), std::size_t{1}, "следующий целый кадр распознан");
    if (!frames.empty()) check_eq(int(frames[0].payload[0]), 55, "распознан именно целый кадр");
}

void test_rejects_bad_checksum() {
    begin_test("Ресинхронизация — испорченный payload");

    std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, makePixels(4, 10));
    frame[7] ^= 0xFFu;  // портим байт внутри payload

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);
    const std::vector<DecodedFrame> frames = decodeAll(frame, &decoder);

    check(frames.empty(), "кадр с несошедшимся xor отброшен");
    check_eq(decoder.frames_bad_checksum, 1u, "счётчик плохих сумм увеличился");
    check_eq(decoder.frames_ok, 0u, "счётчик хороших кадров не тронут");

    /* И после отказа декодер продолжает работать. */
    const std::vector<DecodedFrame> after =
        decodeAll(encode(LED_FRAME_PIXELS, makePixels(2, 77)), &decoder);
    check_eq(after.size(), std::size_t{1}, "следующий кадр после сбоя распознан");
    check_eq(decoder.frames_ok, 1u, "счётчик хороших кадров пошёл");
}

void test_rejects_absurd_length() {
    begin_test("Ресинхронизация — неправдоподобная длина");

    /* Заголовок собрался из случайных байт и обещает 60000 байт payload.
     * Ждать их — значит проглотить все следующие настоящие кадры. */
    std::vector<std::uint8_t> stream = {0xAA, 0x55, 0x01, 0x60, 0xEA};

    const std::vector<std::uint8_t> good = encode(LED_FRAME_PIXELS, makePixels(2, 33));
    stream.insert(stream.end(), good.begin(), good.end());

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);
    const std::vector<DecodedFrame> frames = decodeAll(stream, &decoder);

    check_eq(decoder.frames_bad_length, 1u, "неправдоподобная длина отвергнута сразу");
    check_eq(frames.size(), std::size_t{1}, "настоящий кадр следом распознан");
    if (!frames.empty()) check_eq(int(frames[0].payload[0]), 33, "и он не испорчен");
}

void test_magic_inside_payload() {
    begin_test("Ресинхронизация — магия внутри данных");

    /* 0xAA 0x55 может встретиться в самих пикселях. Декодер не должен на это
     * реагировать: он в это время читает payload по счётчику длины. */
    std::vector<std::uint8_t> payload = {0xAA, 0x55, 0x01, 0xB0, 0x00, 0xAA, 0x55, 0xAA, 0x55};
    const std::vector<DecodedFrame> frames = decodeAll(encode(LED_FRAME_PIXELS, payload));

    check_eq(frames.size(), std::size_t{1}, "распознан один кадр, а не несколько");
    if (!frames.empty()) check(frames[0].payload == payload, "payload с магией внутри цел");
}

void test_double_magic_prefix() {
    begin_test("Ресинхронизация — 0xAA перед настоящей магией");

    /* Байт 0xAA пришёл как мусор, следом настоящий кадр. Если декодер после
     * несовпадения откатится в самое начало, он съест 0xAA настоящего кадра
     * и потеряет его целиком. */
    std::vector<std::uint8_t> stream = {0xAA, 0xAA};
    const std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, makePixels(1, 11));
    stream.insert(stream.end(), frame.begin(), frame.end());

    const std::vector<DecodedFrame> frames = decodeAll(stream);
    check_eq(frames.size(), std::size_t{1}, "кадр после лишних 0xAA не потерян");
    if (!frames.empty()) check_eq(int(frames[0].payload[0]), 11, "payload верный");
}

void test_byte_by_byte_delivery() {
    begin_test("Поток — байты приходят вразнобой");

    /* USB CDC отдаёт данные кусками произвольного размера: декодер обязан быть
     * инкрементальным и не зависеть от того, как поток нарезан. */
    const std::vector<std::uint8_t> frame = encode(LED_FRAME_PIXELS, makePixels(144, 3));

    led_proto_decoder_t decoder{};
    led_proto_decoder_init(&decoder);

    int completed = 0;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (led_proto_decoder_push(&decoder, frame[i])) {
            ++completed;
            check_eq(i, frame.size() - 1, "кадр завершился ровно на последнем байте");
        }
    }
    check_eq(completed, 1, "побайтовая подача дала ровно один кадр");
}

}  // namespace

int main() {
    std::cout << "Тесты протокола кадра (общий код Mac и прошивки)\n";

    test_encode_layout();
    test_encode_length_field();
    test_encode_rejects_bad_args();
    test_roundtrip();
    test_roundtrip_all_types();
    test_back_to_back_frames();
    test_recovers_after_garbage();
    test_recovers_after_truncated_frame();
    test_rejects_bad_checksum();
    test_rejects_absurd_length();
    test_magic_inside_payload();
    test_double_magic_prefix();
    test_byte_by_byte_delivery();

    return finish();
}

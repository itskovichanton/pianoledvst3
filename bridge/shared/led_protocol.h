/**
 * Протокол кадра между Mac и ESP32-C6.
 *
 * Один и тот же файл компилируется обеими сторонами: на Mac как C++ (в плагине),
 * в прошивке как C. Поэтому здесь чистый C89-совместимый интерфейс без единой
 * зависимости — ни ESP-IDF, ни JUCE, ни стандартной библиотеки сверх stdint.
 *
 * Формат кадра:
 *
 *   0xAA 0x55 | type | len_lo len_hi | payload[len] | xor
 *   \___________/      \___________/                  \___/
 *     магия             длина payload         xor всех байт payload
 *
 * Почему так, а не сложнее:
 *   - USB CDC уже несёт свой CRC и переспрашивает потерянное, поэтому от
 *     контрольной суммы здесь нужно только одно — поймать РАССИНХРОН (начали
 *     читать с середины кадра, хост перезапустился, прошивка перезагрузилась).
 *     С этим xor справляется, а кадры идут каждые 8 мс, так что даже
 *     пропущенный кадр не виден глазом.
 *   - Длина есть, хотя payload почти всегда фиксированный: она позволяет одной
 *     прошивке работать с лентами разной длины и добавлять новые типы кадров,
 *     не ломая приёмник.
 *
 * Порядок байт в payload кадра пикселей — R, G, B на светодиод. Перестановку в
 * GRB, которую хочет WS2812, делает прошивка через led_strip_set_pixel(): так
 * порядок каналов задан ровно в одном месте, и его нельзя перепутать молча.
 */

#ifndef LED_PROTOCOL_H
#define LED_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_PROTO_MAGIC0 0xAAu
#define LED_PROTO_MAGIC1 0x55u

/** Размер заголовка: магия(2) + тип(1) + длина(2). */
#define LED_PROTO_HEADER_SIZE 5u

/** Заголовок + контрольная сумма — накладные расходы на кадр. */
#define LED_PROTO_OVERHEAD (LED_PROTO_HEADER_SIZE + 1u)

/** Потолок payload. 512 светодиодов * 3 байта с запасом. */
#define LED_PROTO_MAX_PAYLOAD 1600u

/** Версия протокола — прошивка сообщает её в PONG. */
#define LED_PROTO_VERSION 1u

typedef enum {
    /** Mac -> ESP32. Кадр пикселей: payload = len/3 светодиодов по R,G,B. */
    LED_FRAME_PIXELS = 0x01,

    /** Mac -> ESP32. Проверка связи, payload пуст. */
    LED_FRAME_PING = 0x02,

    /** ESP32 -> Mac. Ответ: версия(1) + led_count(2, little-endian). */
    LED_FRAME_PONG = 0x03,

    /** ESP32 -> Mac. Диагностика текстом. Консоль на C6 занята этим же USB,
     *  поэтому ESP_LOG туда писать нельзя — он изрубил бы поток кадров. */
    LED_FRAME_LOG = 0x04,

    /** Mac -> ESP32. Погасить всю ленту, payload пуст. */
    LED_FRAME_CLEAR = 0x05
} led_frame_type_t;

/** Размер payload у PONG. */
#define LED_PROTO_PONG_PAYLOAD_SIZE 3u

/**
 * Кодирует кадр в буфер.
 *
 * @param type     тип кадра (led_frame_type_t)
 * @param payload  данные; может быть NULL при len == 0
 * @param len      длина payload, не больше LED_PROTO_MAX_PAYLOAD
 * @param out      буфер назначения
 * @param out_cap  вместимость out
 * @return число записанных байт, либо 0 при некорректных аргументах или
 *         нехватке места (частичный кадр никогда не пишется).
 */
size_t led_proto_encode(uint8_t type, const uint8_t* payload, uint16_t len, uint8_t* out,
                        size_t out_cap);

/** Сколько байт займёт кадр с payload длиной len. */
size_t led_proto_encoded_size(uint16_t len);

/* ── Инкрементальный декодер ──────────────────────────────────────────
 *
 * Автомат, которому скармливают байты по одному, как они приходят из порта.
 * Держит всё в себе, не аллоцирует, не зовёт коллбэки — поэтому одинаково
 * пригоден и для потока на Mac, и для прерывания в прошивке.
 *
 * Восстановление после мусора устроено так: любая неудача (незнакомая длина,
 * несошедшийся xor) возвращает автомат в поиск магии. Отдельно обрабатывается
 * случай, когда байт после 0xAA снова 0xAA — это может быть началом настоящего
 * кадра, и терять его нельзя.
 */

typedef enum {
    LED_DEC_WAIT_MAGIC0 = 0,
    LED_DEC_WAIT_MAGIC1,
    LED_DEC_TYPE,
    LED_DEC_LEN_LO,
    LED_DEC_LEN_HI,
    LED_DEC_PAYLOAD,
    LED_DEC_CHECKSUM
} led_proto_decoder_state_t;

typedef struct {
    led_proto_decoder_state_t state;
    uint8_t type;
    uint16_t length;
    uint16_t received;
    uint8_t checksum;
    uint8_t payload[LED_PROTO_MAX_PAYLOAD];

    /* Счётчики для диагностики: видно, сыпется ли линия. */
    uint32_t frames_ok;
    uint32_t frames_bad_checksum;
    uint32_t frames_bad_length;
} led_proto_decoder_t;

/** Приводит декодер в исходное состояние. Вызывать перед первым push. */
void led_proto_decoder_init(led_proto_decoder_t* decoder);

/**
 * Скармливает один байт.
 * @return 1, если этим байтом кадр завершён — тогда type/length/payload
 *         декодера содержат готовый кадр и валидны до следующего push.
 *         0 во всех остальных случаях.
 */
int led_proto_decoder_push(led_proto_decoder_t* decoder, uint8_t byte);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LED_PROTOCOL_H */

#include "led_protocol.h"

size_t led_proto_encoded_size(uint16_t len) {
    return (size_t)len + LED_PROTO_OVERHEAD;
}

size_t led_proto_encode(uint8_t type, const uint8_t* payload, uint16_t len, uint8_t* out,
                        size_t out_cap) {
    size_t i;
    uint8_t checksum = 0;

    if (out == NULL) return 0;
    if (len > LED_PROTO_MAX_PAYLOAD) return 0;
    if (len > 0 && payload == NULL) return 0;
    if (out_cap < led_proto_encoded_size(len)) return 0;

    out[0] = LED_PROTO_MAGIC0;
    out[1] = LED_PROTO_MAGIC1;
    out[2] = type;
    out[3] = (uint8_t)(len & 0xFFu);
    out[4] = (uint8_t)((len >> 8) & 0xFFu);

    for (i = 0; i < (size_t)len; ++i) {
        out[LED_PROTO_HEADER_SIZE + i] = payload[i];
        checksum = (uint8_t)(checksum ^ payload[i]);
    }

    out[LED_PROTO_HEADER_SIZE + (size_t)len] = checksum;
    return led_proto_encoded_size(len);
}

void led_proto_decoder_init(led_proto_decoder_t* decoder) {
    if (decoder == NULL) return;

    decoder->state = LED_DEC_WAIT_MAGIC0;
    decoder->type = 0;
    decoder->length = 0;
    decoder->received = 0;
    decoder->checksum = 0;
    decoder->frames_ok = 0;
    decoder->frames_bad_checksum = 0;
    decoder->frames_bad_length = 0;
}

int led_proto_decoder_push(led_proto_decoder_t* decoder, uint8_t byte) {
    if (decoder == NULL) return 0;

    switch (decoder->state) {
        case LED_DEC_WAIT_MAGIC0:
            if (byte == LED_PROTO_MAGIC0) decoder->state = LED_DEC_WAIT_MAGIC1;
            return 0;

        case LED_DEC_WAIT_MAGIC1:
            if (byte == LED_PROTO_MAGIC1) {
                decoder->state = LED_DEC_TYPE;
            } else if (byte == LED_PROTO_MAGIC0) {
                /* 0xAA 0xAA 0x55 — второй 0xAA может быть настоящим началом
                 * кадра, поэтому остаёмся здесь, а не откатываемся в поиск. */
                decoder->state = LED_DEC_WAIT_MAGIC1;
            } else {
                decoder->state = LED_DEC_WAIT_MAGIC0;
            }
            return 0;

        case LED_DEC_TYPE:
            decoder->type = byte;
            decoder->state = LED_DEC_LEN_LO;
            return 0;

        case LED_DEC_LEN_LO:
            decoder->length = byte;
            decoder->state = LED_DEC_LEN_HI;
            return 0;

        case LED_DEC_LEN_HI:
            decoder->length = (uint16_t)(decoder->length | ((uint16_t)byte << 8));
            if (decoder->length > LED_PROTO_MAX_PAYLOAD) {
                /* Почти наверняка мы читаем не заголовок, а середину чужих
                 * данных — начинаем искать магию заново. */
                ++decoder->frames_bad_length;
                decoder->state = LED_DEC_WAIT_MAGIC0;
                return 0;
            }
            decoder->received = 0;
            decoder->checksum = 0;
            decoder->state = (decoder->length == 0) ? LED_DEC_CHECKSUM : LED_DEC_PAYLOAD;
            return 0;

        case LED_DEC_PAYLOAD:
            decoder->payload[decoder->received] = byte;
            decoder->checksum = (uint8_t)(decoder->checksum ^ byte);
            ++decoder->received;
            if (decoder->received >= decoder->length) decoder->state = LED_DEC_CHECKSUM;
            return 0;

        case LED_DEC_CHECKSUM:
            decoder->state = LED_DEC_WAIT_MAGIC0;
            if (byte == decoder->checksum) {
                ++decoder->frames_ok;
                return 1;
            }
            ++decoder->frames_bad_checksum;
            return 0;

        default:
            decoder->state = LED_DEC_WAIT_MAGIC0;
            return 0;
    }
}

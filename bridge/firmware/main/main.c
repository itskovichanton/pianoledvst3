/**
 * Прошивка ESP32-C6: светодиодная лента как «тупой дисплей».
 *
 * Про ноты, клавиши и MIDI эта прошивка не знает НИЧЕГО. Она принимает по USB
 * готовый кадр пикселей и выводит его на ленту. Вся музыкальная логика живёт в
 * плагине на Mac — там её можно править и перезапускать за секунды, тогда как
 * каждое изменение здесь стоит перепрошивки.
 *
 * Две вещи прошивка всё же решает сама, потому что она физически рядом с лентой:
 *
 *   1. Лимит тока. Что бы ни прислал хост, кадр будет притушен до безопасного
 *      уровня — см. led_guard.c.
 *   2. Гашение при пропаже хоста. Закрыли Logic, выдернули провод, упал
 *      плагин — лента не должна остаться гореть навсегда.
 *
 * ВАЖНО про консоль: у C6 порт USB Serial/JTAG один, и по нему же идут кадры.
 * Любой ESP_LOG вклинился бы прямо в середину кадра, поэтому консоль в
 * sdkconfig.defaults отключена (CONFIG_ESP_CONSOLE_NONE), а диагностика
 * уходит хосту отдельным типом кадра — LED_FRAME_LOG.
 */

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "led_guard.h"
#include "led_protocol.h"

/* ── Конфигурация установки ───────────────────────────────────────── */

#define LED_GPIO 4     /* DIN ленты */
#define LED_COUNT 144  /* светодиодов в метре ленты */

/**
 * Бюджет тока на ленту, мА.
 *
 * Плата не готова отдавать больше 200 мА суммарно; сам ESP32-C6 без активного
 * радио берёт ориентировочно 30-50 мА, остальное отдаём ленте. Значение
 * намеренно консервативное: превысить его физически невозможно, а поднять —
 * вопрос одной строки, если появится внешнее питание.
 */
#define CURRENT_BUDGET_MA 150u

/** Через сколько молчания хоста гасить ленту. */
#define HOST_TIMEOUT_US (2 * 1000 * 1000)

/** Потолок ожидания при записи хосту: если хост не читает, не зависаем. */
#define TX_TIMEOUT_MS 20

/* ── Состояние ────────────────────────────────────────────────────── */

static led_strip_handle_t s_strip = NULL;
static led_proto_decoder_t s_decoder;
static uint8_t s_pixels[LED_COUNT * 3];
static uint8_t s_tx[LED_PROTO_MAX_PAYLOAD + LED_PROTO_OVERHEAD];

/* Счётчики для ответа на диагностические запросы. */
static uint32_t s_frames_applied;
static uint32_t s_frames_clamped;

/* ── Отправка хосту ───────────────────────────────────────────────── */

static void send_frame(uint8_t type, const uint8_t* payload, uint16_t length) {
    const size_t size = led_proto_encode(type, payload, length, s_tx, sizeof(s_tx));
    if (size == 0) return;
    usb_serial_jtag_write_bytes(s_tx, size, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(TX_TIMEOUT_MS));
}

static void send_log(const char* text) {
    send_frame(LED_FRAME_LOG, (const uint8_t*)text, (uint16_t)strlen(text));
}

static void send_pong(void) {
    const uint8_t payload[LED_PROTO_PONG_PAYLOAD_SIZE] = {
        LED_PROTO_VERSION, (uint8_t)(LED_COUNT & 0xFF), (uint8_t)((LED_COUNT >> 8) & 0xFF)};
    send_frame(LED_FRAME_PONG, payload, sizeof(payload));
}

/* ── Вывод на ленту ───────────────────────────────────────────────── */

static void push_pixels_to_strip(void) {
    if (s_strip == NULL) return;
    /* Порядок каналов задан ровно здесь и больше нигде: по проводу идёт RGB,
     * а перестановку в GRB, которую хочет WS2812, делает драйвер внутри
     * led_strip_set_pixel(). Так цвета невозможно перепутать молча. */
    for (int led = 0; led < LED_COUNT; ++led) {
        led_strip_set_pixel(s_strip, led, s_pixels[led * 3 + 0], s_pixels[led * 3 + 1],
                            s_pixels[led * 3 + 2]);
    }
    led_strip_refresh(s_strip);
}

static void blank_strip(void) {
    memset(s_pixels, 0, sizeof(s_pixels));
    if (s_strip != NULL) led_strip_clear(s_strip);
}

static void apply_pixel_frame(const uint8_t* payload, uint16_t length) {
    size_t usable;

    if ((length % 3u) != 0u) {
        send_log("кадр отброшен: длина не кратна 3");
        return;
    }

    /* Хост может быть настроен на ленту другой длины. Берём столько, сколько
     * помещается, остаток гасим — так рассинхрон настроек виден глазом
     * (часть ленты просто тёмная), но ничего не ломается. */
    usable = length;
    if (usable > sizeof(s_pixels)) usable = sizeof(s_pixels);

    memcpy(s_pixels, payload, usable);
    if (usable < sizeof(s_pixels)) {
        memset(s_pixels + usable, 0, sizeof(s_pixels) - usable);
    }

    if (led_guard_apply(s_pixels, sizeof(s_pixels), CURRENT_BUDGET_MA)) {
        ++s_frames_clamped;
        /* Сообщаем не каждый раз, а на первом срабатывании и далее изредка:
         * поток кадров идёт до 120 раз в секунду, и болтливый лог забил бы
         * канал сильнее, чем сами кадры. */
        if (s_frames_clamped == 1u || (s_frames_clamped % 500u) == 0u) {
            send_log("кадр притушен до лимита тока");
        }
    }

    push_pixels_to_strip();
    ++s_frames_applied;
}

/* ── Точка входа ──────────────────────────────────────────────────── */

void app_main(void) {
    usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 1024,
    };

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = {.invert_out = false},
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {.with_dma = false},
    };

    uint8_t rx[256];
    int64_t last_contact_us;
    bool blanked = false;
    esp_err_t strip_err;

    /* USB раньше ленты: если RMT не поднялся, хост всё равно получает PONG
     * и лог, а не мёртвый порт после reboot-loop из ESP_ERROR_CHECK. */
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    led_proto_decoder_init(&s_decoder);
    last_contact_us = esp_timer_get_time();
    send_log("piano-led usb");

    strip_err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (strip_err != ESP_OK) {
        s_strip = NULL;
        send_log("strip init failed");
    } else {
        led_strip_clear(s_strip);
        send_log("piano-led ready");
    }

    for (;;) {
        /* Таймаут короткий не ради скорости реакции на кадры (их приносит сам
         * read), а чтобы цикл регулярно просыпался и проверял тишину хоста. */
        const int read = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(50));

        for (int i = 0; i < read; ++i) {
            if (!led_proto_decoder_push(&s_decoder, rx[i])) continue;

            switch (s_decoder.type) {
                case LED_FRAME_PIXELS:
                    apply_pixel_frame(s_decoder.payload, s_decoder.length);
                    last_contact_us = esp_timer_get_time();
                    blanked = false;
                    break;

                case LED_FRAME_CLEAR:
                    blank_strip();
                    last_contact_us = esp_timer_get_time();
                    blanked = true;
                    break;

                case LED_FRAME_PING:
                    send_pong();
                    last_contact_us = esp_timer_get_time();
                    break;

                default:
                    /* Незнакомый тип — молча пропускаем: так хост с более новой
                     * версией протокола не сломает старую прошивку. */
                    break;
            }
        }

        /* Хост пропал — гасим ленту. Без этого закрытый Logic оставил бы
         * последний аккорд гореть до отключения питания. */
        if (!blanked && (esp_timer_get_time() - last_contact_us) > HOST_TIMEOUT_US) {
            blank_strip();
            blanked = true;
            send_log("хост молчит — лента погашена");
        }
    }
}

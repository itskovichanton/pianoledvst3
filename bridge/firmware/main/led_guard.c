#include "led_guard.h"

uint32_t led_guard_estimate_ua(const uint8_t* pixels, size_t byte_count) {
    uint32_t total_ua = 0;
    size_t i;

    if (pixels == NULL) return 0;

    /* Каждый канал линейно: value/255 от полного тока канала.
     * Максимум для нашего кадра — 432 байта * 20000 мкА = 8.64 А в мкА,
     * это 8.6e6, до переполнения uint32 далеко. */
    for (i = 0; i < byte_count; ++i) {
        total_ua += ((uint32_t)pixels[i] * LED_GUARD_CHANNEL_UA) / 255u;
    }
    return total_ua;
}

int led_guard_apply(uint8_t* pixels, size_t byte_count, uint32_t budget_ma) {
    uint32_t estimated_ua;
    uint32_t budget_ua;
    uint32_t scale_q16;
    size_t i;

    if (pixels == NULL || byte_count == 0) return 0;

    if (budget_ma == 0) {
        for (i = 0; i < byte_count; ++i) pixels[i] = 0;
        return 1;
    }

    estimated_ua = led_guard_estimate_ua(pixels, byte_count);
    budget_ua = budget_ma * 1000u;

    if (estimated_ua <= budget_ua) return 0;

    /* Коэффициент в формате Q16: доля бюджета от запрошенного тока.
     * Числитель считаем в 64 битах — budget_ua << 16 при 150 мА это уже
     * 9.8e9 и в uint32 не помещается. */
    scale_q16 = (uint32_t)(((uint64_t)budget_ua << 16) / estimated_ua);

    for (i = 0; i < byte_count; ++i) {
        pixels[i] = (uint8_t)(((uint32_t)pixels[i] * scale_q16) >> 16);
    }
    return 1;
}

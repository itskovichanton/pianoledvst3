#include "piano_led/frame_builder.h"

#include <algorithm>
#include <cmath>

namespace piano_led {
namespace {

/* ~20 мА на канал при полной яркости — справочное значение для WS2812B. */
constexpr double kChannelCurrentMa = 20.0;

}  // namespace

FrameBuilder::FrameBuilder(StripLayout layout) : layout_(std::move(layout)) {
    if (layout_.ledCount < 0) layout_.ledCount = 0;
    frame_.assign(static_cast<std::size_t>(layout_.ledCount) * 3u, 0u);
}

void FrameBuilder::clear() {
    std::fill(frame_.begin(), frame_.end(), std::uint8_t{0});
}

Rgb LedStyle::toRgb() const {
    const float percent = std::clamp(brightnessPercent, 0.1f, 20.0f);
    const int peak = std::max(1, static_cast<int>(std::lround(percent * 255.0f / 100.0f)));

    float h = hue;
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    const float s = std::clamp(saturation, 0.0f, 1.0f);

    const float chroma = s;
    const float hp = h / 60.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (hp < 1.0f) {
        r1 = chroma;
        g1 = x;
    } else if (hp < 2.0f) {
        r1 = x;
        g1 = chroma;
    } else if (hp < 3.0f) {
        g1 = chroma;
        b1 = x;
    } else if (hp < 4.0f) {
        g1 = x;
        b1 = chroma;
    } else if (hp < 5.0f) {
        r1 = x;
        b1 = chroma;
    } else {
        r1 = chroma;
        b1 = x;
    }
    const float m = 1.0f - chroma;
    r1 += m;
    g1 += m;
    b1 += m;

    const auto scale = [peak](float value) -> std::uint8_t {
        return static_cast<std::uint8_t>(std::lround(value * static_cast<float>(peak)));
    };
    return {scale(r1), scale(g1), scale(b1)};
}

void FrameBuilder::lightLed(int index, Rgb color) {
    if (index < 0 || index >= layout_.ledCount || frame_.empty()) return;
    const std::size_t base = static_cast<std::size_t>(index) * 3u;
    if (base + 2 >= frame_.size()) return;
    frame_[base + 0] = color.r;
    frame_[base + 1] = color.g;
    frame_[base + 2] = color.b;
}

void FrameBuilder::lightCenter(int count, Rgb color) {
    if (layout_.ledCount <= 0 || count <= 0 || frame_.empty()) return;
    if (count > layout_.ledCount) count = layout_.ledCount;
    const int start = (layout_.ledCount - count) / 2;
    for (int i = 0; i < count; ++i) lightLed(start + i, color);
}

void FrameBuilder::lightNote(int midiNote, Rgb color) {
    if (!layout_.covers(midiNote) || frame_.empty()) return;

    const int keyIndex = midiNote - layout_.lowestNote;
    const int count = layout_.sizeForKey(keyIndex);
    const int start = layout_.ledStartForKey(keyIndex);

    for (int i = 0; i < count; ++i) {
        int led = start + i;
        if (layout_.reversed) led = layout_.ledCount - 1 - led;
        if (led < 0 || led >= layout_.ledCount) continue;

        const std::size_t base = static_cast<std::size_t>(led) * 3u;
        frame_[base + 0] = color.r;
        frame_[base + 1] = color.g;
        frame_[base + 2] = color.b;
    }
}

void FrameBuilder::build(const NoteBitmask::Snapshot& notes, Rgb color) {
    std::fill(frame_.begin(), frame_.end(), std::uint8_t{0});

    if (!layout_.isValid()) return;

    const int lowest = layout_.lowestNote;
    const int highest = layout_.highestNote();

    for (int note = lowest; note <= highest; ++note) {
        if (!notes.isOn(note)) continue;
        lightNote(note, color);
    }
}

double FrameBuilder::estimatedCurrentMa() const {
    double total = 0.0;
    for (const std::uint8_t channel : frame_) {
        total += (static_cast<double>(channel) / 255.0) * kChannelCurrentMa;
    }
    return total;
}

}  // namespace piano_led

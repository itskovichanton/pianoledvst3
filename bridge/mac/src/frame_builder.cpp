#include "piano_led/frame_builder.h"

#include <algorithm>

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

void FrameBuilder::lightLed(int index, Rgb color) {
    if (index < 0 || index >= layout_.ledCount || frame_.empty()) return;
    const std::size_t base = static_cast<std::size_t>(index) * 3u;
    if (base + 2 >= frame_.size()) return;
    frame_[base + 0] = color.r;
    frame_[base + 1] = color.g;
    frame_[base + 2] = color.b;
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

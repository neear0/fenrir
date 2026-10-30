#include "../ui.h"

// slider_float(): a thin track filled in the accent up to the value, with a knob that grows and glows while it's
// hovered or dragged, the value at the end of the track and the label after it

bool celosia::widgets::slider_float(const char* label, float* v, float min, float max, const char* format) {
    const ImVec2 padding = style::frame::size_padding, inner = style::general::inner_spacing;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = celosia::render::text::size(label, celosia::render::text::label_end(label));
    const ImVec2 pos = core::cursor();
    const ImRect frame(pos, pos + ImVec2(core::item_width(), core::frame_height())); // as wide and tall as a combo
    const ImRect total(pos, frame.Max + ImVec2(label_size.x > 0.f ? inner.x + label_size.x : 0.f, 0.f));
    core::layout(total, padding.y);
    if (!core::add(total, id))
        return false;

    // the track leaves room for the widest value, so it doesn't change length while the value's text does
    char value_text[64];
    ImFormatString(value_text, IM_ARRAYSIZE(value_text), format, min);
    float value_width = celosia::render::text::size(value_text).x;
    ImFormatString(value_text, IM_ARRAYSIZE(value_text), format, max);
    value_width = ImMax(value_width, celosia::render::text::size(value_text).x);
    ImFormatString(value_text, IM_ARRAYSIZE(value_text), format, *v);

    const float scale = celosia::style::scale;
    const float knob_radius = 7.f * scale; // while held, smaller otherwise
    const float track_min = frame.Min.x + knob_radius;
    const float track_max = ImMax(track_min, frame.Max.x - value_width - inner.x * 2.f - knob_radius);

    bool hovered, held;
    core::button(frame, id, &hovered, &held);
    bool changed = false;
    // the keyboard steps it by a hundredth of its range, a tenth with shift
    const float key_step = (max - min) * (input::key_down(keys::shift) ? 0.1f : 0.01f);
    const float keyed = ImClamp(*v - (core::focus_key(id, keys::left) ? key_step : 0.f) + (core::focus_key(id, keys::right) ? key_step : 0.f), ImMin(min, max), ImMax(min, max));
    if (keyed != *v) {
        *v = keyed;
        changed = true;
    }
    if (held && track_max > track_min) { // the value follows the cursor while the slider is held, also outside of it
        const float value = ImLerp(min, max, ImSaturate((input::mouse_pos().x - track_min) / (track_max - track_min)));
        if (value != *v) {
            *v = value;
            changed = true;
        }
    }

    ImDrawList* drawlist = core::drawlist();
    const ImVec4 main = celosia::style::general::accent.Value;
    const ImVec4 text = celosia::style::themes::active.text.Value;
    const float t = max > min ? ImSaturate((*v - min) / (max - min)) : 0.f;
    const ImVec2 knob(ImLerp(track_min, track_max, t), frame.GetCenter().y);
    const float grow = celosia::animations::set(celosia::animations::key("slider_knob", id), held ? 1.f : hovered ? 0.6f : 0.f);
    const float radius = ImLerp(5.f * scale, knob_radius, grow);

    // the track in the text color, faint, so it shows over the os blur and on both themes
    const float track_half = 2.f * scale;
    const float track_alpha = celosia::animations::set(celosia::animations::key("slider_track", id), hovered || held ? 0.22f : 0.12f);
    drawlist->AddRectFilled(ImVec2(track_min, knob.y - track_half), ImVec2(track_max, knob.y + track_half), ImColor(text.x, text.y, text.z, track_alpha), track_half);
    drawlist->AddRectFilled(ImVec2(track_min, knob.y - track_half), ImVec2(knob.x, knob.y + track_half), celosia::style::general::accent, track_half);

    if (grow > 0.01f) // glows in the accent like a hovered button
        celosia::effects::glow(drawlist, knob - ImVec2(radius, radius), knob + ImVec2(radius, radius), ImColor(main.x, main.y, main.z, 0.5f * grow), radius, 8.f * scale);
    drawlist->AddCircleFilled(knob, radius, celosia::style::general::accent);
    drawlist->AddCircle(knob, radius, ImColor(text.x, text.y, text.z, 0.9f), 0, 2.f * scale);

    const ImVec2 value_size = celosia::render::text::size(value_text);
    celosia::render::text::draw(core::drawlist(), ImVec2(frame.Max.x - value_size.x, knob.y - value_size.y * 0.5f), celosia::style::themes::active.text, value_text);
    if (label_size.x > 0.f)
        celosia::render::text::draw(core::drawlist(), ImVec2(frame.Max.x + inner.x, knob.y - label_size.y * 0.5f), celosia::style::themes::active.text, label, celosia::render::text::label_end(label));

    return changed;
}

#include "../ui.h"

// combo(): a frame showing the picked item with an arrow, opening a frosted popup under it (above when there's no room
// below) that lists the items, at most max_items rows of them before it scrolls

static float popup_height(int items, float padding) { // of a popup that shows that many items, padding above and below them
    const float spacing = celosia::style::general::spacing.y;
    return (celosia::core::font_size() + spacing) * items - spacing + padding * 2.f;
}

static bool selectable(const char* label, bool selected) { // an item in the popup, as wide as it, highlighted when picked or hovered
    using namespace celosia;
    const ImVec2 spacing = style::general::spacing;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = render::text::size(label, render::text::label_end(label));
    const ImVec2 pos = core::cursor() + ImVec2(0.f, core::text_offset());
    core::layout(label_size, 0.f);

    // the text stays where it is, the box spans the rest of the line and the spacing around it, the items meet without a gap to click into
    const ImVec2 text_min = pos, text_max(pos.x + ImMax(label_size.x, core::line_end() - pos.x), pos.y + label_size.y);
    ImRect bb(text_min, text_max);
    const float spacing_left = IM_TRUNC(spacing.x * 0.5f), spacing_up = IM_TRUNC(spacing.y * 0.5f);
    bb.Min -= ImVec2(spacing_left, spacing_up);
    bb.Max += ImVec2(spacing.x - spacing_left, spacing.y - spacing_up);
    if (!core::add(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = core::button(bb, id, &hovered, &held);

    ImColor col = celosia::style::window::background_color();
    if (selected) col = celosia::style::general::accent;
    if (hovered) col = celosia::style::themes::active.hovered;
    if (pressed || held) col = celosia::style::themes::rgb_intensity(celosia::style::general::accent, 0.5f);
    celosia::widgets::render_frame_animated(id, bb.Min, bb.Max, col);
    celosia::render::text::draw_aligned(core::drawlist(), text_min, text_max, ImVec2(0.f, 0.f), celosia::style::themes::active.text, label, celosia::render::text::label_end(label), &label_size, &bb);

    if (pressed && core::popup_level() > 0)
        popup::close_current();
    return pressed;
}

bool celosia::widgets::combo(const char* label, int* current, const char* const items[], int count, int max_items) {
    const ImVec2 padding = style::frame::size_padding, inner = style::general::inner_spacing;
    const float rounding = (float)style::general::rounding;
    const ImGuiID id = core::id(label);
    const float arrow_size = core::frame_height();
    const ImVec2 label_size = render::text::size(label, render::text::label_end(label));
    const ImRect frame(core::cursor(), core::cursor() + ImVec2(core::item_width(), label_size.y + padding.y * 2.f));
    const ImRect total(frame.Min, frame.Max + ImVec2(label_size.x > 0.f ? inner.x + label_size.x : 0.f, 0.f));
    core::layout(total, padding.y);
    if (!core::add(total, id))
        return false;

    bool hovered, held;
    const bool pressed = core::button(frame, id, &hovered, &held);
    const ImGuiID popup_id = ImHashStr("##ComboPopup", 0, id);
    bool popup_open = popup::is_open(popup_id);
    if (pressed && !popup_open) {
        popup::open(popup_id);
        popup_open = true;
    }

    // outlined like a button, the outline and the arrow in the accent while it's open
    const style::themes::t_theme& theme = style::themes::active;
    const ImVec4 text = theme.text.Value;
    ImVec4 border = ImLerp(hovered ? theme.hovered.Value : theme.idle.Value, text, 0.12f);
    border.w = theme.idle.Value.w;
    ImVec4 arrow = ImVec4(text.x, text.y, text.z, 0.7f);
    if (popup_open) border = arrow = style::general::accent.Value;
    if (held) border = theme.held.Value;

    const float value_x2 = ImMax(frame.Min.x, frame.Max.x - arrow_size);
    core::drawlist()->AddRectFilled(frame.Min, ImVec2(value_x2, frame.Max.y), theme.idle, rounding, ImDrawFlags_RoundCornersLeft);
    if (value_x2 + arrow_size - padding.x <= frame.Max.x)
        ImGui::RenderArrow(core::drawlist(), ImVec2(value_x2 + padding.y, frame.Min.y + padding.y), ImColor(arrow), ImGuiDir_Down, 1.f);
    render_frame_border_animated(id, frame.Min, frame.Max, ImColor(border), rounding);

    if (*current >= 0 && *current < count)
        render::text::draw_aligned(core::drawlist(), frame.Min + padding, ImVec2(value_x2, frame.Max.y), ImVec2(0.f, 0.f), style::themes::active.text, items[*current], render::text::label_end(items[*current]));
    if (label_size.x > 0.f)
        render::text::draw(core::drawlist(), ImVec2(frame.Max.x + inner.x, frame.Min.y + padding.y), style::themes::active.text, label, render::text::label_end(label));

    // at least as wide as the frame, with the items' text lined up with the frame's
    const ImVec2 popup_padding(padding.x, 0.f);
    if (!popup_open || !popup::begin(popup_id, frame, popup_padding, frame.GetWidth(), popup_height(max_items > 0 ? max_items : 8, popup_padding.y)))
        return false;

    bool changed = false;
    for (int i = 0; i < count; i++) {
        core::push_id(i);
        const bool picked = i == *current;
        if (picked)
            core::default_focus(core::id(items[i])); // the keyboard starts at the picked item
        if (selectable(items[i], picked) && !picked) {
            *current = i;
            changed = true;
        }
        core::pop_id();
    }
    popup::end();

    return changed;
}

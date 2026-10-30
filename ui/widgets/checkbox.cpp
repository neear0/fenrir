#include "../ui.h"

// the box's border in the text color, faint, so an unchecked box shows over the os blur and on both themes. checked, the border
// and the mark ease into the accent, unchecked the mark fades out
static void render_check_mark_animated(ImGuiID id, bool active, bool hovered, bool held, ImDrawList* drawlist, ImRect check_bb, float pad, float square_sz) {
    const ImVec4 text = celosia::style::themes::active.text.Value;
    ImVec4 border = ImVec4(text.x, text.y, text.z, held ? 0.3f : hovered ? 0.45f : 0.25f);
    ImVec4 mark = celosia::style::general::accent.Value;
    if (active)
        border = mark;
    else
        mark.w = 0.f;

    const ImColor border_animated = celosia::animations::set(celosia::animations::key("checkbox_border", id), border, celosia::animations::color_speed);
    const ImColor mark_animated = celosia::animations::set(celosia::animations::key("checkbox", id), mark, celosia::animations::color_speed);

    ImGui::RenderCheckMark(drawlist, check_bb.Min + ImVec2(pad, pad), mark_animated, square_sz - pad * 2.0f);
    celosia::widgets::render_frame_border_color(check_bb.Min, check_bb.Max, border_animated, (float)celosia::style::general::rounding);
}

bool celosia::widgets::checkbox(const char* label, bool* v) {
    const ImVec2 padding = style::frame::size_padding, inner = style::general::inner_spacing;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = celosia::render::text::size(label, celosia::render::text::label_end(label));

    const float square_sz = IM_TRUNC(core::font_size() * 1.15f); // follows the text size, not the frame padding
    const float row_height = ImMax(square_sz, label_size.y); // the box and the label are centered on the row
    const ImVec2 pos = core::cursor();
    const ImRect total_bb(pos, pos + ImVec2(square_sz + (label_size.x > 0.0f ? inner.x + label_size.x : 0.0f), row_height + padding.y));
    core::layout(total_bb, padding.y);
    if (!core::add(total_bb, id))
        return false;

    bool hovered, held;
    const bool pressed = core::button(total_bb, id, &hovered, &held);
    if (pressed)
        *v = !*v;

    const ImVec2 check_pos(pos.x, pos.y + IM_TRUNC((row_height - square_sz) * 0.5f));
    const ImRect check_bb(check_pos, check_pos + ImVec2(square_sz, square_sz));

    const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
    render_check_mark_animated(id, *v, hovered, held, core::drawlist(), check_bb, pad, square_sz);

    ImVec2 label_pos = ImVec2(check_bb.Max.x + inner.x, pos.y + IM_TRUNC((row_height - label_size.y) * 0.5f));
    if (label_size.x > 0.0f)
        celosia::render::text::draw(core::drawlist(), label_pos, celosia::style::themes::active.text, label, celosia::render::text::label_end(label));

    return pressed;
}

bool celosia::widgets::checkbox_map(const char* label) { return checkbox(label, &celosia::variables::config::bools[label]); }

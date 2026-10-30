#include "../ui.h"

static void style1(ImGuiID id, const ImRect bb, const char* label, ImVec2 label_size, ImColor col) {
    const float rounding = (float)celosia::style::general::rounding;
    const ImVec2 padding = celosia::style::frame::size_padding;
    ImVec4 fill = ImLerp(col.Value, celosia::style::themes::active.background_darker.Value, 0.25f); // a bit towards the panel color, darker than the border on dark themes and lighter on light ones
    fill.w = col.Value.w;
    celosia::widgets::render_frame_animated(id, bb.Min, bb.Max, fill, rounding);
    ImVec4 border = ImLerp(col.Value, celosia::style::themes::active.text.Value, 0.12f); // a little towards the text color, so the button shows over a dark desktop through the os blur
    border.w = col.Value.w;
    celosia::widgets::render_frame_border_animated(id, bb.Min, bb.Max, border, rounding);
    celosia::render::text::draw_aligned(celosia::core::drawlist(), bb.Min + padding, bb.Max - padding, ImVec2(0.5f, 0.5f), celosia::style::themes::active.text, label, celosia::render::text::label_end(label), &label_size, &bb);
}

bool celosia::widgets::button(const char* label, const ImVec2& size_arg) {
    const ImVec2 padding = style::frame::size_padding;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = celosia::render::text::size(label, celosia::render::text::label_end(label));

    const ImVec2 pos = core::cursor();
    const ImVec2 size = core::item_size(size_arg, label_size.x + padding.x * 2.0f, label_size.y + padding.y * 2.0f);

    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    core::layout(size, padding.y);
    if (!core::add(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = core::button(bb, id, &hovered, &held);

    const float glow = celosia::animations::set(celosia::animations::key("button_glow", id), hovered ? 1.f : 0.f);
    if (glow > 0.01f) { // hovered buttons glow faintly in the accent
        const ImVec4 main = celosia::style::general::accent.Value;
        celosia::effects::glow(core::drawlist(), bb.Min, bb.Max, ImColor(main.x, main.y, main.z, 0.35f * glow), (float)style::general::rounding, 8.f * celosia::style::scale);
    }

    const style::themes::t_theme& theme = style::themes::active;
    const ImColor col = (held && hovered) ? theme.held : hovered ? theme.hovered : theme.idle;
    style1(id, bb, label, label_size, col);

    return pressed;
}

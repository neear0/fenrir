#include "../ui.h"

// segmented(): side by side choices sharing one frame as wide as a slider, with the label after it. a pill in the accent
// slides to the picked one and fades out while nothing is picked (current -1). text over the pill is dark or white,
// whichever stands out more from the accent

bool celosia::widgets::segmented(const char* label, int* current, const char* const items[], int count) {
    if (count <= 0)
        return false;

    const ImVec2 padding = style::frame::size_padding, inner = style::general::inner_spacing;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = celosia::render::text::size(label, celosia::render::text::label_end(label));
    const ImVec2 pos = core::cursor();
    const ImRect frame(pos, pos + ImVec2(core::item_width(), core::frame_height()));
    const ImRect total(pos, frame.Max + ImVec2(label_size.x > 0.f ? inner.x + label_size.x : 0.f, 0.f));
    core::layout(total, padding.y);
    if (!core::add(total, id, core::item_no_focus)) // the segments take the keyboard
        return false;

    ImDrawList* drawlist = core::drawlist();
    const ImVec4 text = celosia::style::themes::active.text.Value;
    const ImVec4 accent = celosia::style::general::accent.Value;
    const float segment = frame.GetWidth() / (float)count;
    const float scale = celosia::style::scale;
    const float rounding = (float)style::general::rounding;

    // the frame like a button's: the idle color with an outline a little towards the text color
    const ImVec4 fill = style::themes::active.idle.Value;
    ImVec4 border = ImLerp(fill, text, 0.12f);
    border.w = fill.w;
    drawlist->AddRectFilled(frame.Min, frame.Max, ImColor(fill), rounding);
    drawlist->AddRect(frame.Min, frame.Max, ImColor(border), rounding, 0, celosia::style::frame::border_size);

    bool changed = false;
    for (int i = 0; i < count; i++) {
        const ImRect rect(ImVec2(frame.Min.x + segment * i, frame.Min.y), ImVec2(frame.Min.x + segment * (i + 1), frame.Max.y));
        const ImGuiID item = ImHashData(&i, sizeof(i), id); // from the control's id, two controls in one window don't share their segments
        if (!core::add(rect, item))
            continue;
        bool hovered, held;
        if (core::button(rect, item, &hovered, &held) && *current != i) {
            *current = i;
            changed = true;
        }
        const float hover = celosia::animations::set(celosia::animations::key("segmented_hover", item), hovered && *current != i ? 1.f : 0.f, celosia::animations::color_speed);
        if (hover > 0.01f)
            drawlist->AddRectFilled(rect.Min, rect.Max, ImColor(text.x, text.y, text.z, 0.08f * hover), rounding);
    }

    // the pill keeps its place while nothing is picked, so it fades out where it was. a hidden one appears where it's
    // going instead of sliding in from wherever it was last
    const ImGuiID pill_key = celosia::animations::key("segmented_pill", id), alpha_key = celosia::animations::key("segmented_pill_alpha", id);
    const float pill_goal = segment * *current; // from the frame's left edge, so it doesn't trail behind when the groupbox scrolls
    if (*current >= 0 && celosia::animations::get(alpha_key) < 0.01f)
        celosia::animations::snap(pill_key, pill_goal);
    const float pill_x = celosia::animations::set(pill_key, *current >= 0 ? pill_goal : celosia::animations::get(pill_key), 14.f);
    const float pill_alpha = celosia::animations::set(alpha_key, *current >= 0 ? 1.f : 0.f, celosia::animations::color_speed);
    const float inset = 3.f * scale;
    if (pill_alpha > 0.01f) {
        const ImVec2 pill_min(frame.Min.x + pill_x + inset, frame.Min.y + inset), pill_max(frame.Min.x + pill_x + segment - inset, frame.Max.y - inset);
        celosia::effects::glow(drawlist, pill_min, pill_max, ImColor(accent.x, accent.y, accent.z, 0.35f * pill_alpha), rounding, 6.f * scale);
        drawlist->AddRectFilled(pill_min, pill_max, ImColor(accent.x, accent.y, accent.z, pill_alpha), ImMax(rounding - inset * 0.5f, 0.f));
    }

    const ImVec4 on_accent = celosia::style::themes::text_on(accent).Value;
    for (int i = 0; i < count; i++) { // each label turns to the text on the accent as far as the pill covers its segment
        const float x = frame.Min.x + segment * i;
        const float covered = ImSaturate(1.f - fabsf(pill_x - segment * i) / segment) * pill_alpha;
        const ImVec2 size = celosia::render::text::size(items[i]);
        const ImVec2 at(x + (segment - size.x) * 0.5f, frame.GetCenter().y - size.y * 0.5f);
        drawlist->PushClipRect(ImVec2(x, frame.Min.y), ImVec2(x + segment, frame.Max.y), true);
        celosia::render::text::draw(drawlist, at, ImColor(ImLerp(text, on_accent, covered)), items[i]);
        drawlist->PopClipRect();
    }
    if (label_size.x > 0.f)
        celosia::render::text::draw(core::drawlist(), ImVec2(frame.Max.x + inner.x, frame.GetCenter().y - label_size.y * 0.5f), celosia::style::themes::active.text, label, celosia::render::text::label_end(label));

    return changed;
}

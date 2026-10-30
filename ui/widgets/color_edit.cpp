#include "../ui.h"

// color_edit(): a swatch that opens a frosted popup with color_picker() in it.
// color_picker(): saturation/value square, hue bar and alpha bar. the gradients are rounded shapes whose vertices get
// their colors from shade(), the gradients are linear so any triangulation of the shape gets them exact

static void shade(ImDrawList* drawlist, int vertex_start, ImVec2 p0, ImVec2 p1, ImVec4 c0, ImVec4 c1) { // the vertices added since vertex_start go from c0 at p0 to c1 at p1, alpha included
    const ImVec2 direction = p1 - p0;
    const float length_sqr = ImMax(ImLengthSqr(direction), 1.f);
    for (int i = vertex_start; i < drawlist->VtxBuffer.Size; i++) {
        ImDrawVert& vertex = drawlist->VtxBuffer[i];
        ImVec4 color = ImLerp(c0, c1, ImSaturate(ImDot(vertex.pos - p0, direction) / length_sqr));
        color.w *= (float)((vertex.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f; // keeps the antialiased edge
        vertex.col = ImGui::ColorConvertFloat4ToU32(color);
    }
}

static void gradient_rect(ImDrawList* drawlist, const ImRect& rect, ImVec2 p0, ImVec2 p1, ImVec4 c0, ImVec4 c1, float rounding, ImDrawFlags flags = 0) {
    const int start = drawlist->VtxBuffer.Size;
    drawlist->AddRectFilled(rect.Min, rect.Max, IM_COL32_WHITE, rounding, flags);
    shade(drawlist, start, p0, p1, c0, c1);
}

static bool drag_area(const char* id, const ImRect& rect, ImVec2* position) { // position is 0 to 1 across the rect while it's held
    const ImGuiID item = celosia::core::id(id);
    if (!celosia::core::add(rect, item, celosia::core::item_no_focus))
        return false;
    bool hovered, held;
    celosia::core::button(rect, item, &hovered, &held, celosia::core::item_no_focus);
    if (!held)
        return false;
    const ImVec2 mouse = celosia::input::mouse_pos();
    *position = ImVec2(ImSaturate((mouse.x - rect.Min.x) / rect.GetWidth()), ImSaturate((mouse.y - rect.Min.y) / rect.GetHeight()));
    return true;
}

static void bar_handle(ImDrawList* drawlist, const ImRect& bar, float t) { // across a vertical bar at t (0 top, 1 bottom)
    const float y = ImLerp(bar.Min.y, bar.Max.y, t);
    const float scale = celosia::style::scale;
    const ImVec2 top_left(bar.Min.x - 2.f * scale, y - 3.f * scale), bottom_right(bar.Max.x + 2.f * scale, y + 3.f * scale);
    drawlist->AddRect(top_left - ImVec2(1, 1), bottom_right + ImVec2(1, 1), IM_COL32(0, 0, 0, 90), 4.f * scale, 0, 1.f);
    drawlist->AddRect(top_left, bottom_right, IM_COL32_WHITE, 3.f * scale, 0, 2.f * scale);
}

bool celosia::widgets::color_picker(const char* label, float col[4], bool alpha, float saturation_min, float value_min) {
    ImDrawList* drawlist = core::drawlist();
    ImGuiStorage* storage = core::storage();
    core::push_id(label);

    // grey has no hue and black no saturation either, the last ones picked are kept for as long as the color is the one they made
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(col[0], col[1], col[2], h, s, v);
    const ImGuiID id_hue = core::id("##hue_saved"), id_sat = core::id("##sat_saved"), id_color = core::id("##color_saved");
    if ((ImU32)storage->GetInt(id_color, 0) == ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], 1.f))) {
        if (s == 0.f || v == 0.f)
            h = storage->GetFloat(id_hue, h);
        if (v == 0.f)
            s = storage->GetFloat(id_sat, s);
    }

    const float scale = celosia::style::scale;
    const float square = 160.f * scale, bar = 14.f * scale, gap = 8.f * scale;
    const ImVec2 pos = core::cursor();
    const ImRect sv_rect(pos, pos + ImVec2(square, square));
    const ImRect hue_rect(ImVec2(sv_rect.Max.x + gap, pos.y), ImVec2(sv_rect.Max.x + gap + bar, sv_rect.Max.y));
    const ImRect alpha_rect(ImVec2(hue_rect.Max.x + gap, pos.y), ImVec2(hue_rect.Max.x + gap + bar, sv_rect.Max.y));
    const ImRect total(pos, alpha ? alpha_rect.Max : hue_rect.Max);
    core::layout(total);

    bool changed = false;
    ImVec2 p;
    if (drag_area("##sv", sv_rect, &p)) {
        s = ImLerp(saturation_min, 1.f, p.x);
        v = ImLerp(1.f, value_min, p.y);
        changed = true;
    }
    if (drag_area("##hue", hue_rect, &p)) {
        h = ImMin(p.y, 0.9999f); // 1 would wrap around to red at the top
        changed = true;
    }
    if (alpha && drag_area("##alpha", alpha_rect, &p)) {
        col[3] = 1.f - p.y;
        changed = true;
    }
    if (changed) {
        ImGui::ColorConvertHSVtoRGB(h, s, v, col[0], col[1], col[2]);
        storage->SetFloat(id_hue, h);
        storage->SetFloat(id_sat, s);
        storage->SetInt(id_color, (int)ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], 1.f)));
    }

    const float rounding = (float)celosia::style::general::rounding;
    ImVec4 hue_color(0, 0, 0, 1);
    ImGui::ColorConvertHSVtoRGB(h, 1.f, 1.f, hue_color.x, hue_color.y, hue_color.z);
    const ImVec4 opaque(col[0], col[1], col[2], 1.f);

    // saturation left to right, value top to bottom, each from its min. a color is v * lerp(white, hue, s), linear in both,
    // so a white to hue gradient under a black one that gets opaque by 1 - v is exact
    const ImVec4 least_saturated = ImLerp(ImVec4(1, 1, 1, 1), hue_color, saturation_min);
    gradient_rect(drawlist, sv_rect, sv_rect.Min, ImVec2(sv_rect.Max.x, sv_rect.Min.y), least_saturated, hue_color, rounding);
    gradient_rect(drawlist, sv_rect, sv_rect.Min, ImVec2(sv_rect.Min.x, sv_rect.Max.y), ImVec4(0, 0, 0, 0), ImVec4(0, 0, 0, 1.f - value_min), rounding);
    const float marker_s = saturation_min < 1.f ? ImSaturate((s - saturation_min) / (1.f - saturation_min)) : 0.f;
    const float marker_v = value_min < 1.f ? ImSaturate((1.f - v) / (1.f - value_min)) : 0.f;
    const ImVec2 marker(ImLerp(sv_rect.Min.x, sv_rect.Max.x, marker_s), ImLerp(sv_rect.Min.y, sv_rect.Max.y, marker_v));
    drawlist->AddCircleFilled(marker, 6.f * scale, ImGui::ColorConvertFloat4ToU32(opaque));
    drawlist->AddCircle(marker, 7.f * scale, IM_COL32(0, 0, 0, 90), 0, 1.f);
    drawlist->AddCircle(marker, 6.f * scale, IM_COL32_WHITE, 0, 2.f * scale);

    // hue, six segments between the primary and secondary colors, the ends rounded
    static const ImVec4 hues[7] = { {1,0,0,1}, {1,1,0,1}, {0,1,0,1}, {0,1,1,1}, {0,0,1,1}, {1,0,1,1}, {1,0,0,1} };
    for (int i = 0; i < 6; i++) {
        const ImRect segment(ImVec2(hue_rect.Min.x, ImLerp(hue_rect.Min.y, hue_rect.Max.y, i / 6.f)), ImVec2(hue_rect.Max.x, ImLerp(hue_rect.Min.y, hue_rect.Max.y, (i + 1) / 6.f)));
        const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersTop : i == 5 ? ImDrawFlags_RoundCornersBottom : ImDrawFlags_RoundCornersNone;
        gradient_rect(drawlist, segment, segment.Min, ImVec2(segment.Min.x, segment.Max.y), hues[i], hues[i + 1], rounding, corners);
    }
    bar_handle(drawlist, hue_rect, h);

    if (alpha) { // opaque at the top, see-through at the bottom, over a checkerboard
        ImGui::RenderColorRectWithAlphaCheckerboard(drawlist, alpha_rect.Min, alpha_rect.Max, IM_COL32(0, 0, 0, 0), bar / 2.f, ImVec2(0, 0), rounding);
        gradient_rect(drawlist, alpha_rect, alpha_rect.Min, ImVec2(alpha_rect.Min.x, alpha_rect.Max.y), opaque, ImVec4(opaque.x, opaque.y, opaque.z, 0.f), rounding);
        bar_handle(drawlist, alpha_rect, 1.f - col[3]);
    }

    core::pop_id();
    return changed;
}

bool celosia::widgets::color_edit(const char* label, float col[4], bool alpha, float saturation_min, float value_min) {
    const ImVec2 padding = style::frame::size_padding, inner = style::general::inner_spacing;
    const float rounding = (float)style::general::rounding;
    const ImGuiID id = core::id(label);
    const ImVec2 label_size = celosia::render::text::size(label, celosia::render::text::label_end(label));
    const float size = core::frame_height();
    const ImVec2 pos = core::cursor();
    const ImRect swatch(pos, pos + ImVec2(size, size));
    const ImRect total(pos, pos + ImVec2(size + (label_size.x > 0.f ? inner.x + label_size.x : 0.f), size));
    core::layout(total, padding.y);
    if (!core::add(total, id))
        return false;

    bool hovered, held;
    const bool pressed = core::button(total, id, &hovered, &held);

    const ImVec4 color(col[0], col[1], col[2], alpha ? col[3] : 1.f);
    const float glow = celosia::animations::set(celosia::animations::key("color_edit_glow", id), hovered ? 1.f : 0.f);
    if (glow > 0.01f) // glows in its own color while hovered
        celosia::effects::glow(core::drawlist(), swatch.Min, swatch.Max, ImColor(color.x, color.y, color.z, 0.5f * glow), rounding, 8.f * celosia::style::scale);
    ImGui::RenderColorRectWithAlphaCheckerboard(core::drawlist(), swatch.Min, swatch.Max, ImGui::ColorConvertFloat4ToU32(color), size / 4.f, ImVec2(0, 0), rounding);
    core::drawlist()->AddRect(swatch.Min, swatch.Max, IM_COL32(255, 255, 255, (int)(40 + 60 * glow)), rounding);
    if (label_size.x > 0.f)
        celosia::render::text::draw(core::drawlist(), ImVec2(swatch.Max.x + inner.x, pos.y + (size - label_size.y) * 0.5f), celosia::style::themes::active.text, label, celosia::render::text::label_end(label));

    core::push_id(label);
    const ImGuiID popup_id = core::id("##picker");
    if (pressed)
        popup::open(popup_id);

    bool changed = false;
    const ImRect anchor(swatch.Min, swatch.Max + ImVec2(0.f, style::general::spacing.y + 4.f * celosia::style::scale)); // a little gap under the swatch
    if (popup::begin(popup_id, anchor, celosia::style::general::padding)) {
        changed = color_picker("##body", col, alpha, saturation_min, value_min);
        const ImU32 rgba = ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], col[3]));
        char hex[16];
        if (alpha)
            ImFormatString(hex, IM_ARRAYSIZE(hex), "#%02X%02X%02X%02X", rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF, rgba >> 24);
        else
            ImFormatString(hex, IM_ARRAYSIZE(hex), "#%02X%02X%02X", rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF);
        widgets::text(hex);
        popup::end();
    }
    core::pop_id();
    return changed;
}

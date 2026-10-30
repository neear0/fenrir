#include "../ui.h"

// the surfaces widgets go on: the window the ui fills, scroll areas inside it and popups over it, on core's surfaces.
// scrolling is smooth: the mouse wheel and dragging the scrollbar set where a surface scrolls to, begin_surface() eases
// its scroll there

using celosia::core::t_surface;

void celosia::window::begin(const char* name, ImVec2 pos, ImVec2 size) {
    core::begin_surface(ImHashStr(name), t_surface::window, pos, ImTrunc(size), ImVec2(0.f, 0.f));
}

void celosia::window::end() {
    core::end_surface();
}

void celosia::scroll_area::begin(ImGuiID id, ImVec2 size) {
    t_surface* parent = core::surface();
    // on whole pixels, end() lays it out from where it ends up
    t_surface* area = core::begin_surface(ImHashData(&id, sizeof(id), parent->id), t_surface::scroll_area, parent->cursor, ImTrunc(size), ImVec2(0.f, 0.f));
    parent->cursor = area->pos;
}

void celosia::scroll_area::begin(const char* name, ImVec2 size) {
    begin(core::id(name), size);
}

void celosia::scroll_area::end() {
    const ImVec2 size = core::surface()->size;
    core::end_surface();
    core::layout(size);
}

void celosia::core::scroll_by(t_surface* s, float notches) {
    const float step = IM_TRUNC(ImMin(5.f * font_size(), s->inner.GetHeight() * 0.67f)) * notches * style::general::scroll_multiplier;
    // from where it's heading, so quick wheel ticks add up. without a scrollbar there's nothing to ease
    s->scroll_target = ImClamp((s->scrollbar ? s->scroll_target : s->scroll) - step, 0.f, s->scroll_max);
}

void celosia::core::scrollbar(t_surface* s) {
    static float click_to_grab_center = 0.f; // where on the grab it was clicked, it stays there under the mouse while dragged

    const float width = style::scrollbar::width;
    const ImRect frame(ImMax(s->pos.x, s->pos.x + s->size.x - width), s->inner.Min.y, s->pos.x + s->size.x, s->inner.Max.y);
    if (frame.GetWidth() <= 0.f || frame.GetHeight() <= 0.f)
        return;
    // too short to grab, it fades out
    float alpha = 1.f;
    if (frame.GetHeight() < font_size() + style::frame::size_padding.y * 2.f)
        alpha = ImSaturate((frame.GetHeight() - font_size()) / (style::frame::size_padding.y * 2.f));
    if (alpha <= 0.f)
        return;

    ImRect bb = frame;
    bb.Expand(ImVec2(-ImClamp(IM_TRUNC((frame.GetWidth() - 2.f) * 0.5f), 0.f, 3.f), -ImClamp(IM_TRUNC((frame.GetHeight() - 2.f) * 0.5f), 0.f, 3.f)));
    const float track = bb.GetHeight();

    // the grab is as big as the part that shows, never shorter than grab_min
    const ImS64 visible = (ImS64)s->inner.GetHeight(), total = (ImS64)(s->content.y + s->padding.y * 2.f);
    const ImS64 total_or_visible = ImMax(ImMax(total, visible), (ImS64)1);
    const float grab_height = ImClamp(track * ((float)visible / (float)total_or_visible), style::scrollbar::grab_min, track);
    const float grab_norm = grab_height / track;

    const ImGuiID id = ImHashStr("#SCROLLY", 0, s->id);
    add(frame, id, item_no_focus);
    bool hovered, held;
    button(bb, id, &hovered, &held, item_no_focus);

    const ImS64 scroll_max = ImMax((ImS64)1, total - visible);
    ImS64 scroll = (ImS64)s->scroll;
    float grab_v = ImSaturate((float)scroll / (float)scroll_max) * (track - grab_height) / track;
    if (held && alpha >= 1.f && grab_norm < 1.f) {
        const float clicked = ImSaturate((input::mouse_pos().y - bb.Min.y) / track);
        hover(id);
        bool seek = false; // clicked off the grab, it jumps there
        if (activated(id)) {
            seek = clicked < grab_v || clicked > grab_v + grab_norm;
            click_to_grab_center = seek ? 0.f : clicked - grab_v - grab_norm * 0.5f;
        }
        const float scroll_norm = ImSaturate((clicked - click_to_grab_center - grab_norm * 0.5f) / (1.f - grab_norm));
        scroll = (ImS64)(scroll_norm * scroll_max);
        grab_v = ImSaturate((float)scroll / (float)scroll_max) * (track - grab_height) / track;
        if (seek)
            click_to_grab_center = clicked - grab_v - grab_norm * 0.5f;
        s->scroll_target = scroll_norm * scroll_max;
    }

    // ImGui's dark style greys for hovered and held, on both themes
    ImDrawList* drawlist = s->parent ? &s->parent->drawlist : &s->drawlist; // a scroll area's goes into what it's in, before its content
    const float rounding = s->kind == t_surface::popup ? (float)style::general::rounding_popup : 0.f;
    drawlist->AddRectFilled(frame.Min, frame.Max, style::themes::active.background_darker, rounding, ImDrawFlags_RoundCornersTopRight | ImDrawFlags_RoundCornersBottomRight);
    ImVec4 grab = held ? ImVec4(0.51f, 0.51f, 0.51f, 1.f) : hovered ? ImVec4(0.41f, 0.41f, 0.41f, 1.f) : style::themes::active.idle.Value;
    grab.w *= alpha;
    const ImRect grab_rect(bb.Min.x, ImLerp(bb.Min.y, bb.Max.y, grab_v), bb.Max.x, ImLerp(bb.Min.y, bb.Max.y, grab_v) + grab_height);
    drawlist->AddRectFilled(grab_rect.Min, grab_rect.Max, ImColor(animations::set(animations::key("scrollbar_grab", s->id), grab, animations::color_speed)), style::scrollbar::rounding);
}

void celosia::popup::open(ImGuiID id) {
    core::open_popup(id);
}

bool celosia::popup::is_open(ImGuiID id) {
    return core::popup_open(id);
}

static ImVec2 fit_size(ImVec2 content, ImVec2 padding, float min_width, float max_height) { // what's in it with the padding around it, within the limits
    const bool constrained = min_width > 0.f || max_height < FLT_MAX;
    const auto constrain = [&](ImVec2 size) {
        if (constrained)
            size = ImTrunc(ImVec2(ImMax(size.x, min_width), ImMin(size.y, max_height)));
        return ImMax(size, ImVec2(4.f, 4.f));
    };
    const ImVec2 room = celosia::ui::display_size - ImVec2(celosia::style::general::popup_margin, celosia::style::general::popup_margin) * 2.f;
    ImVec2 size = ImClamp(content + padding * 2.f, ImVec2(4.f, 4.f), ImMax(ImVec2(4.f, 4.f), room));
    if (constrain(size).y - padding.y * 2.f < content.y) // it'll scroll, the scrollbar takes room next to the content
        size.x += celosia::style::scrollbar::width;
    return constrain(size);
}

static ImVec2 place(const ImRect& anchor, ImVec2 size) { // under the anchor, above it when there's no room below, inside the window's edges
    const float margin = celosia::style::general::popup_margin;
    ImRect room(ImVec2(0.f, 0.f), celosia::ui::display_size);
    room.Expand(ImVec2(room.GetWidth() > margin * 2.f ? -margin : 0.f, room.GetHeight() > margin * 2.f ? -margin : 0.f));
    const ImVec2 tries[] = {
        ImVec2(anchor.Min.x, anchor.Max.y),                   // below, from the left edge
        ImVec2(anchor.Min.x, anchor.Min.y - size.y),          // above, from the left edge
        ImVec2(anchor.Max.x - size.x, anchor.Max.y),          // below, from the right edge
        ImVec2(anchor.Max.x - size.x, anchor.Min.y - size.y), // above, from the right edge
    };
    for (const ImVec2& pos : tries)
        if (room.Contains(ImRect(pos, pos + size)))
            return pos;
    ImVec2 pos = anchor.GetBL(); // nowhere it fits, kept on screen as far as it goes
    pos.x = ImMax(ImMin(pos.x + size.x, room.Max.x) - size.x, room.Min.x);
    pos.y = ImMax(ImMin(pos.y + size.y, room.Max.y) - size.y, room.Min.y);
    return pos;
}

bool celosia::popup::begin(ImGuiID id, const ImRect& anchor, ImVec2 padding, float min_width, float max_height) {
    if (!is_open(id))
        return false;

    // sized to what it held last frame. the frame it opens it isn't shown, there's nothing to size it by yet
    const t_surface* existing = core::find(id);
    const ImVec2 size = fit_size(existing ? existing->content : ImVec2(0.f, 0.f), padding, min_width, max_height);
    const ImVec2 pos = existing && existing->frame == core::frame() - 1 ? place(anchor, size) : anchor.GetBL();

    core::begin_popup();
    t_surface* surface = core::begin_surface(id, t_surface::popup, pos, size, padding);

    // frosted glass over whatever is behind the popup, all the way to its edges
    ImDrawList* drawlist = &surface->drawlist;
    const float rounding = (float)style::general::rounding_popup;
    ImColor tint = style::themes::active.background_darker;
    tint.Value.w = 0.6f;
    drawlist->PushClipRect(surface->pos, surface->pos + surface->size);
    effects::blur(drawlist, surface->pos, surface->pos + surface->size, tint, rounding, style::window::glass_blur);
    effects::noise(drawlist, surface->pos, surface->pos + surface->size, style::window::noise, rounding);
    drawlist->PopClipRect();
    return true;
}

void celosia::popup::end() {
    core::end_surface();
    core::end_popup();
}

void celosia::popup::close_current() {
    core::close_popup();
}

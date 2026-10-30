#include "../ui.h"

// virtual_list(): a list as long as it needs to be in a scroll area, drawing only the rows that can be seen. the scroll
// area's content is set to the height of all the rows, so the scrollbar and the wheel behave as if every row was there.
// the list is a single item for the keyboard and the mouse: a click picks the row under the mouse and puts the keyboard on
// the list, the arrows then move the selection instead of the keyboard (core::focus_key), up from the first row and down
// from the last one leave the list like they would any other item.
// rows are on float positions, past about 16 million pixels of rows (600k rows of 28) they start to land between pixels

bool celosia::widgets::virtual_list(const char* label, ImVec2 size_arg, int count, float row_height, int* selected, const t_draw_row& draw_row) {
    const ImGuiID id = core::id(label);
    const ImVec2 pos = core::cursor();
    const ImVec2 size = core::item_size(size_arg, core::available().x, row_height * 8.f);
    const ImRect frame(pos, pos + size);
    count = ImMax(count, 0);
    if (*selected >= count)
        *selected = count - 1;

    // the keyboard comes to the list in the surface it's in, so its ring isn't cut off by the list's own clip
    if (!core::add(frame, id)) {
        core::layout(size);
        return false;
    }

    bool changed = false, scroll_to_selected = false;
    const auto select = [&](int index) {
        if (index != *selected) {
            *selected = index;
            changed = true;
        }
        scroll_to_selected = true;
    };
    if (count > 0) {
        if (*selected < count - 1 && core::focus_key(id, keys::down))
            select(*selected + 1);
        if (*selected > 0 && core::focus_key(id, keys::up))
            select(*selected - 1);
        if (core::focus_key(id, keys::home))
            select(0);
        if (core::focus_key(id, keys::end))
            select(count - 1);
    }

    // the frame like segmented()'s: the idle color with an outline a little towards the text color
    ImDrawList* drawlist_parent = core::drawlist();
    const ImVec4 fill = style::themes::active.idle.Value;
    ImVec4 border = ImLerp(fill, style::themes::active.text.Value, 0.12f);
    border.w = fill.w;
    const float rounding = (float)style::general::rounding;
    drawlist_parent->AddRectFilled(frame.Min, frame.Max, ImColor(fill), rounding);
    drawlist_parent->AddRect(frame.Min, frame.Max, ImColor(border), rounding, 0, style::frame::border_size);

    scroll_area::begin(id, size);
    core::t_surface* area = core::surface();
    const float top = area->pos.y - area->scroll; // where row 0 is, scrolled along
    const float left = area->work.Min.x, right = area->work.Max.x;
    const ImRect clip = area->clip;

    // the rows in view, from the clip rect, so nothing outside it is ever drawn or even asked for
    const int first = ImClamp((int)ImFloor((clip.Min.y - top) / row_height), 0, count);
    const int last = ImClamp((int)ImFloor((clip.Max.y - top) / row_height) + 1, first, count);

    // one button for all the rows, the row is wherever the mouse is in it
    int hovered_row = -1;
    if (count > 0) {
        ImRect rows(ImVec2(left, ImMax(clip.Min.y, top)), ImVec2(right, ImMin(clip.Max.y, top + row_height * count)));
        if (rows.Min.y < rows.Max.y) {
            bool hovered = false, held = false;
            core::button(rows, id, &hovered, &held);
            if (hovered) {
                hovered_row = ImClamp((int)ImFloor((input::mouse_pos().y - top) / row_height), 0, count - 1);
                if (input::mouse_clicked(input::left))
                    select(hovered_row);
            }
        }
    }

    for (int i = first; i < last; i++) {
        const ImRect rect(ImVec2(left, top + row_height * i), ImVec2(right, top + row_height * (i + 1)));
        draw_row(i, rect, i == *selected, i == hovered_row);
    }

    // the content is as tall as all the rows, the scroll area works out its scrollbar from that
    core::set_cursor(ImVec2(0.f, row_height * count));
    core::layout(ImVec2(0.f, 0.f));

    // the selection moved by the keyboard comes into view, next frame when the scroll is eased there
    if (scroll_to_selected && *selected >= 0) {
        const float y = row_height * *selected, view = area->inner.GetHeight();
        if (y < area->scroll)
            area->scroll_target = y;
        else if (y + row_height > area->scroll + view)
            area->scroll_target = y + row_height - view;
    }

    scroll_area::end();
    return changed;
}

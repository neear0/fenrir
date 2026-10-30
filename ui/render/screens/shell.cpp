#include "../../ui.h"
#include "screens.h"

// the shell around fenrir's pages, drawn with CelosUI's effects and animations:
// - the sidebar is a rail with a thread down all of it: Gleipnir, the ribbon that bound Fenrir. a bead eases along it to
//   the open page with the thread lit around it, and a faint shimmer keeps running down it
// - the open page's item gets a slowly sliding gradient in the accent, a glow and a bar, a hovered one a faint fill
// - the card over Settings says what binds this build: it can't write to a drive
// - the titlebar has the fehu rune for a mark, the page you're on, what this build is, and the window's buttons
// the items are core items like any widget: the keyboard goes through them, ctrl+1 to ctrl+6 jump to a page

using namespace celosia;
using fenrir::icons::e_icon;
namespace screens = fenrir::screens;

namespace {
    constexpr int page_count = IM_ARRAYSIZE(screens::pages);

    float px(float v) { return v * style::scale; } // style sizes are at scale 1
    ImColor with_alpha(ImColor color, float alpha) { color.Value.w *= alpha; return color; }
    ImColor text(float alpha) { return with_alpha(style::themes::active.text, alpha); }
    ImColor accent(float alpha) { return with_alpha(style::general::accent, alpha); }

    int active_page() {
        for (int i = 0; i < page_count; i++)
            if (ui::tab_active == screens::pages[i].name)
                return i;
        return 0;
    }

    // small capitals spread apart, the section headings and the card's title
    void caps(ImDrawList* drawlist, ImVec2 pos, const char* label, ImColor color) {
        ImFont* font = resources::fonts::get("caps");
        for (const char* c = label; *c; c++) {
            const char upper[2] = { *c >= 'a' && *c <= 'z' ? (char)(*c - 'a' + 'A') : *c, 0 };
            render::text::draw(drawlist, pos, color, upper, nullptr, font);
            pos.x += render::text::size(upper, nullptr, font).x + px(1.6f);
        }
    }

    // a rounded pill in the accent: the badges in the sidebar and the chips in the titlebar
    ImVec2 pill_size(const char* label, bool lock) {
        const ImVec2 size = render::text::size(label, nullptr, resources::fonts::get("small"));
        return ImVec2(size.x + px(lock ? 28.f : 14.f), size.y + px(4.f));
    }
    void pill(ImDrawList* drawlist, ImVec2 min, const char* label, bool lock) {
        const ImVec2 size = pill_size(label, lock), max = min + size;
        const float round = size.y * 0.5f;
        drawlist->AddRectFilled(min, max, accent(0.14f), round);
        drawlist->AddRect(min, max, accent(0.5f), round, 0, px(1.f));
        float x = min.x + px(7.f);
        if (lock) {
            fenrir::icons::draw(drawlist, e_icon::lock, ImVec2(x + px(5.f), min.y + size.y * 0.5f), px(11.f), accent(1.f), px(1.4f));
            x += px(14.f);
        }
        render::text::draw(drawlist, ImVec2(x, min.y + px(2.f)), accent(1.f), label, nullptr, resources::fonts::get("small"));
    }

    // a thin vertical gradient on the thread, clipped to where the thread is
    void thread_segment(ImDrawList* drawlist, float x, float y1, float y2, ImColor a, ImColor b, float top, float bottom) {
        if (y2 <= top || y1 >= bottom)
            return;
        const float from = ImMax(y1, top), to = ImMin(y2, bottom);
        const ImColor ca = ImLerp(a.Value, b.Value, (from - y1) / (y2 - y1)), cb = ImLerp(a.Value, b.Value, (to - y1) / (y2 - y1));
        effects::gradient(drawlist, ImVec2(x - px(1.f), from), ImVec2(x + px(1.f), to), ca, cb, 0.f, IM_PI * 0.5f);
    }

    void nav_item(ImDrawList* drawlist, int index, const ImRect& bb) {
        const screens::t_page& page = screens::pages[index];
        const ImGuiID id = core::id(page.name);
        if (!core::add(bb, id))
            return;
        bool hovered = false, held = false;
        if (core::button(bb, id, &hovered, &held))
            ui::tab_active = page.name;

        const bool active = ui::tab_active == page.name;
        const float on = animations::set(animations::key("nav_active", id), active ? 1.f : 0.f, 8.f);
        const float hover = animations::set(animations::key("nav_hover", id), hovered && !active ? 1.f : 0.f, animations::color_speed);
        const float press = animations::set(animations::key("nav_press", id), held && hovered ? 1.f : 0.f, 18.f);
        const float round = px(8.f);

        if (on > 0.01f) {
            effects::glow(drawlist, bb.Min, bb.Max, accent(0.22f * on), round, px(14.f));
            effects::gradient(drawlist, bb.Min, bb.Max, accent(0.32f * on), accent(0.04f * on), round, 0.f, 0.12f);
            drawlist->AddRect(bb.Min, bb.Max, accent(0.35f * on), round, 0, px(1.f));
            drawlist->AddRectFilled(ImVec2(bb.Min.x + px(1.f), bb.Min.y + px(13.f)), ImVec2(bb.Min.x + px(3.5f), bb.Max.y - px(13.f)), accent(on), px(2.f));
        }
        if (hover > 0.01f)
            drawlist->AddRectFilled(bb.Min, bb.Max, text(0.05f * hover), round);
        if (press > 0.01f)
            drawlist->AddRectFilled(bb.Min, bb.Max, text(0.05f * press), round);

        const float lift = ImMax(on, hover);
        const float slide = px(3.f) * on; // the open page's icon and text sit a little to the right
        const ImVec2 icon_center(bb.Min.x + px(22.f) + slide, bb.GetCenter().y);
        fenrir::icons::draw(drawlist, page.icon, icon_center, px(18.f), ImColor(ImLerp(text(0.55f + 0.3f * hover).Value, accent(1.f).Value, on)), px(1.6f));

        ImFont* font_label = resources::fonts::get("default");
        ImFont* font_caption = resources::fonts::get("small");
        const ImVec2 label = render::text::size(page.name, nullptr, font_label);
        const ImVec2 caption = render::text::size(page.caption, nullptr, font_caption);
        const float x = bb.Min.x + px(42.f) + slide;
        const float y = bb.GetCenter().y - (label.y + caption.y) * 0.5f;
        render::text::draw(drawlist, ImVec2(x, y), text(0.72f + 0.28f * lift), page.name, nullptr, font_label);
        render::text::draw(drawlist, ImVec2(x, y + label.y), text(0.40f + 0.12f * lift), page.caption, nullptr, font_caption);
        if (page.badge)
            pill(drawlist, ImVec2(x + label.x + px(8.f), y + (label.y - pill_size(page.badge, false).y) * 0.5f), page.badge, false);

        if (hover > 0.01f) { // the shortcut, on the right while it's hovered
            char shortcut[16];
            snprintf(shortcut, sizeof(shortcut), "Ctrl+%d", index + 1);
            const ImVec2 size = render::text::size(shortcut, nullptr, font_caption);
            render::text::draw(drawlist, ImVec2(bb.Max.x - px(10.f) - size.x, bb.GetCenter().y - size.y * 0.5f), text(0.45f * hover), shortcut, nullptr, font_caption);
        }
    }

    void gleipnir_card(ImDrawList* drawlist, const ImRect& card) {
        const float round = px(10.f);
        drawlist->AddRectFilled(card.Min, card.Max, text(0.035f), round);
        drawlist->AddRect(card.Min, card.Max, accent(0.22f), round, 0, px(1.f));
        // the top edge shimmers, a gradient sliding back and forth along it
        effects::gradient(drawlist, ImVec2(card.Min.x + round, card.Min.y), ImVec2(card.Max.x - round, card.Min.y + px(1.5f)), accent(0.f), accent(0.9f), 0.f, 0.f, 0.3f);

        caps(drawlist, card.Min + ImVec2(px(14.f), px(12.f)), "Gleipnir", text(0.42f));
        fenrir::icons::draw(drawlist, e_icon::lock, card.Min + ImVec2(px(23.f), px(48.f)), px(18.f), accent(1.f), px(1.6f));
        ImFont* font_caption = resources::fonts::get("small");
        render::text::draw(drawlist, card.Min + ImVec2(px(40.f), px(32.f)), text(0.95f), "Read-only build", nullptr, resources::fonts::get("default"));
        render::text::draw(drawlist, card.Min + ImVec2(px(40.f), px(52.f)), text(0.5f), "Can't write to any drive", nullptr, font_caption);
    }
}

void fenrir::shell::sidebar() {
    ImDrawList* drawlist = core::drawlist();
    const float top = style::titlebar::height, width = style::sidebar::width, bottom = ui::size.y;
    if (ui::tab_active.empty()) // nothing picked yet, the first page starts open
        ui::tab_active = ui::tab_current = screens::pages[0].name;

    render::panel(drawlist, ImVec2(0.f, top), ImVec2(width, bottom), style::themes::active.background_darker, (float)style::general::rounding_window);
    drawlist->AddLine(ImVec2(width - 0.5f, top), ImVec2(width - 0.5f, bottom), text(0.07f), 1.f);

    // where everything goes: the list from the top with a heading where a section starts, the bottom ones from the bottom up
    const float margin = px(12.f), item_height = px(48.f), gap = px(4.f);
    const float rail = margin + px(6.f), left = rail + px(12.f), right = width - margin;
    ImRect rects[page_count];
    struct t_heading { float y; const char* label; };
    t_heading headings[page_count];
    int heading_count = 0;
    float y = top + px(14.f);
    for (int i = 0; i < page_count; i++) {
        if (screens::pages[i].bottom)
            continue;
        if (screens::pages[i].section) {
            headings[heading_count++] = { y + px(12.f), screens::pages[i].section };
            y += px(32.f);
        }
        rects[i] = ImRect(left, y, right, y + item_height);
        y += item_height + gap;
    }
    float y_bottom = bottom - margin;
    for (int i = page_count - 1; i >= 0; i--)
        if (screens::pages[i].bottom) {
            rects[i] = ImRect(left, y_bottom - item_height, right, y_bottom);
            y_bottom -= item_height + gap;
        }
    const ImRect card(ImVec2(left, y_bottom - px(80.f)), ImVec2(right, y_bottom - px(4.f)));

    // the thread, from the first item to the last, and the shimmer running down it
    const float thread_top = rects[0].GetCenter().y, thread_bottom = rects[page_count - 1].GetCenter().y;
    drawlist->AddLine(ImVec2(rail, thread_top), ImVec2(rail, thread_bottom), text(0.12f), px(1.5f));
    const float run = thread_bottom - thread_top + px(160.f);
    const float shimmer = thread_top - px(80.f) + fmodf((float)ui::time * px(70.f), run);
    thread_segment(drawlist, rail, shimmer - px(50.f), shimmer, accent(0.f), accent(0.35f), thread_top, thread_bottom);
    thread_segment(drawlist, rail, shimmer, shimmer + px(50.f), accent(0.35f), accent(0.f), thread_top, thread_bottom);

    // the bead eases to the open page, the thread lit around it
    const int active = active_page();
    const float bead = animations::set(animations::key("nav_bead"), rects[active].GetCenter().y, 10.f);
    thread_segment(drawlist, rail, bead - px(90.f), bead, accent(0.f), accent(0.9f), thread_top, thread_bottom);
    thread_segment(drawlist, rail, bead, bead + px(90.f), accent(0.9f), accent(0.f), thread_top, thread_bottom);
    for (int i = 0; i < page_count; i++) // a knot where each item is on the thread
        drawlist->AddCircleFilled(ImVec2(rail, rects[i].GetCenter().y), px(2.5f), text(0.3f));
    const ImVec2 bead_pos(rail, bead);
    effects::glow(drawlist, bead_pos - ImVec2(px(3.f), px(3.f)), bead_pos + ImVec2(px(3.f), px(3.f)), accent(0.9f), px(3.f), px(10.f));
    drawlist->AddCircleFilled(bead_pos, px(4.5f), accent(1.f));
    drawlist->AddCircleFilled(bead_pos, px(1.8f), style::themes::text_on(style::general::accent));

    for (int i = 0; i < heading_count; i++)
        caps(drawlist, ImVec2(left + px(12.f), headings[i].y), headings[i].label, text(0.38f));
    for (int i = 0; i < page_count; i++)
        nav_item(drawlist, i, rects[i]);
    gleipnir_card(drawlist, card);

    if (input::focused() && input::key_down(keys::control)) // ctrl+1 to ctrl+6
        for (int i = 0; i < page_count && i < 9; i++)
            if (input::key_pressed('1' + i, false))
                ui::tab_active = screens::pages[i].name;
}

void fenrir::shell::titlebar() {
    ImDrawList* drawlist = core::drawlist();
    const float height = style::titlebar::height;
    render::panel(drawlist, ImVec2(0.f, 0.f), ImVec2(ui::size.x, height), style::themes::active.background_darker, (float)style::general::rounding_window);
    // a hairline under it, in the accent on the left fading out to the right
    effects::gradient(drawlist, ImVec2(0.f, height - px(1.f)), ImVec2(ui::size.x, height), accent(0.6f), accent(0.f));

    // the mark, glowing, then the name
    const ImVec2 mark(px(26.f), height * 0.5f);
    effects::glow(drawlist, mark - ImVec2(px(5.f), px(9.f)), mark + ImVec2(px(5.f), px(9.f)), accent(0.5f), px(4.f), px(12.f));
    fenrir::icons::fehu(drawlist, mark, px(20.f), accent(1.f), px(2.6f));
    ImFont* font_title = resources::fonts::get("title");
    const ImVec2 name = render::text::size(variables::title.name, nullptr, font_title);
    const ImVec2 rest = render::text::size(variables::title.accent, nullptr, font_title);
    const float name_y = (height - name.y) * 0.5f;
    render::text::draw(drawlist, ImVec2(px(44.f), name_y), text(1.f), variables::title.name, nullptr, font_title);
    render::text::draw(drawlist, ImVec2(px(44.f) + name.x, name_y), accent(1.f), variables::title.accent, nullptr, font_title);
    ImFont* font_small = resources::fonts::get("small");
    const char* tagline = "recovery \xC2\xB7 erasure"; // a middle dot, in utf-8
    const ImVec2 tagline_size = render::text::size(tagline, nullptr, font_small);
    render::text::draw(drawlist, ImVec2(px(44.f) + name.x + rest.x + px(10.f), (height - tagline_size.y) * 0.5f + px(2.f)), text(0.45f), tagline, nullptr, font_small);

    // the page you're on, over the content
    const screens::t_page& page = screens::pages[active_page()];
    ImFont* font_default = resources::fonts::get("default");
    const ImVec2 page_size = render::text::size(page.name, nullptr, font_default);
    const float page_x = style::sidebar::width + style::frame::size_padding.x + px(4.f);
    const float page_y = (height - page_size.y) * 0.5f;
    render::text::draw(drawlist, ImVec2(page_x, page_y), text(0.92f), page.name, nullptr, font_default);
    const ImVec2 caption_size = render::text::size(page.caption, nullptr, font_small);
    render::text::draw(drawlist, ImVec2(page_x + page_size.x + px(10.f), (height - caption_size.y) * 0.5f + px(1.f)), text(0.45f), page.caption, nullptr, font_small);

    // what this build is, right of it, left of the window's buttons
    float x = ui::size.x - px(46.f) * 2.f - px(12.f);
    const char* chips[] = { "P0 build", "Read-only" };
    for (int i = 0; i < 2; i++) {
        const bool lock = i == 1;
        const ImVec2 size = pill_size(chips[i], lock);
        x -= size.x;
        pill(drawlist, ImVec2(x, (height - size.y) * 0.5f), chips[i], lock);
        x -= px(8.f);
    }
}

void fenrir::shell::window_buttons() {
    ImDrawList* drawlist = core::drawlist();
    const float height = style::titlebar::height, width = px(46.f);
    for (int i = 0; i < 2; i++) {
        const bool close = i == 1;
        const ImRect bb(ImVec2(ui::size.x - width * (2 - i), 0.f), ImVec2(ui::size.x - width * (1 - i), height - px(1.f)));
        const ImGuiID id = core::id(close ? "##window_close" : "##window_minimize");
        if (!core::add(bb, id))
            continue;
        bool hovered = false, held = false;
        const bool pressed = core::button(bb, id, &hovered, &held);
        const float hover = animations::set(animations::key("window_button", id), hovered ? 1.f : 0.f, animations::color_speed);
        if (hover > 0.01f)
            drawlist->AddRectFilled(bb.Min, bb.Max, close ? ImColor(0.89f, 0.25f, 0.25f, 0.9f * hover) : text(0.08f * hover));
        const ImColor icon = close ? ImColor(ImLerp(text(0.75f).Value, ImVec4(1.f, 1.f, 1.f, 1.f), hover)) : text(0.75f + 0.25f * hover);
        fenrir::icons::draw(drawlist, close ? e_icon::close : e_icon::minimize, bb.GetCenter(), px(14.f), icon, px(1.4f));
        if (pressed) {
            if (close)
                platform::close();
            else
                platform::minimize();
        }
    }
}

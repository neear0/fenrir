#include "../ui.h"
#include "screens/app.h"
#include "screens/screens.h"

// fenrir's window: CelosUI's background and grain, fenrir's shell (screens/shell.cpp) and the open page from screens/

void celosia::render::main_window() {
    const ImVec2 pos(0.f, 0.f);
    window::begin(variables::title.name, pos, ui::size);

    ImDrawList* drawlist = core::drawlist();
    if (style::window::background == style::window::shader) {
        const ImVec4 main = style::general::accent.Value;
        const ImVec4 accent = style::themes::rgb_accent(style::general::accent).Value;
        ImVec4 base = style::themes::active.background.Value;
        base.w = 1.f; // the window is opaque with the shader background, only the rounded corners let the desktop through
        effects::background(drawlist, pos, pos + celosia::ui::size,
            base, ImColor(main.x, main.y, main.z, 0.55f), ImColor(accent.x, accent.y, accent.z, 0.45f), (float)style::general::rounding_window);
    }
    effects::noise(drawlist, pos, pos + celosia::ui::size, style::window::noise, (float)style::general::rounding_window);
    fenrir::app::frame(); // this frame's snapshot of the session, for every page
    fenrir::shell::sidebar(); // before the titlebar, so the keyboard starts on the pages
    fenrir::shell::titlebar();

    render::tab_content.drawlist = drawlist;
    render::tab_content.vtx_begin = drawlist->VtxBuffer.Size;
    for (const fenrir::screens::t_page& page : fenrir::screens::pages)
        if (ui::tab_current == page.name) // the tab being drawn, see ui::tab_current
            page.draw();
    render::tab_content.vtx_end = drawlist->VtxBuffer.Size;

    fenrir::shell::window_buttons();
    window::end();
}

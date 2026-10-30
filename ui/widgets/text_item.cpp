#include "../ui.h"

// text(): a line of text as an item, in the current font and the theme's text color

void celosia::widgets::text(const char* text) {
    const ImVec2 pos = core::cursor() + ImVec2(0.f, core::text_offset()); // lined up with the text before it on the line
    const ImVec2 size = celosia::render::text::size(text);
    core::layout(size, 0.f);
    if (!core::add(ImRect(pos, pos + size), 0))
        return;
    celosia::render::text::draw(core::drawlist(), pos, celosia::style::themes::active.text, text);
}

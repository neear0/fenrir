#include "../ui.h"

void celosia::render::sidebar() {
	ImDrawList* drawlist_window = core::drawlist();
	render::panel(drawlist_window, ImVec2(0, style::titlebar::height), ImVec2(style::sidebar::width, ui::size.y), style::themes::active.background_darker, (float)style::general::rounding_window);
	core::set_cursor(ImVec2(style::general::padding.x, style::titlebar::height));
	scroll_area::begin("Sidebar", ImVec2(style::sidebar::width - style::general::padding.x, ui::size.y - style::titlebar::height));
	
	render::tabs({ "Widgets", "Appearance" });

	scroll_area::end();
}

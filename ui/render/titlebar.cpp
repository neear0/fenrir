#include "../ui.h"

void celosia::render::titlebar() {
	ImDrawList* drawlist_window = core::drawlist();
	render::panel(drawlist_window, ImVec2(0, 0), ImVec2(ui::size.x, style::titlebar::height), style::themes::active.background_darker, (float)style::general::rounding_window);

	ImFont* font = resources::fonts::get("title");
	const ImVec2 title_size = render::text::size(variables::title.name, nullptr, font);
	const float y_center = (style::titlebar::height - title_size.y) / 2;

	render::text::draw(drawlist_window, ImVec2(style::frame::size_padding.x, y_center), style::themes::active.text, variables::title.name, nullptr, font);
	render::text::draw(drawlist_window, ImVec2(style::frame::size_padding.x + title_size.x, y_center), style::general::accent, variables::title.accent, nullptr, font);
}

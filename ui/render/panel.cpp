#include "../ui.h"

void celosia::render::panel(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor color, float rounding) {
	if (style::window::background == style::window::shader) { // frosted glass over the animated background, tinted with the panel's color
		color.Value.w *= 0.7f; // a lighter tint than the flat fill, or the glass doesn't read as glass
		effects::blur(drawlist, min, max, color, rounding, style::window::glass_blur);
	}
	else { // over the os blur, as see-through as the window's background
		color.Value.w *= style::window::glass_cover();
		drawlist->AddRectFilled(min, max, color, rounding);
	}
	effects::noise(drawlist, min, max, style::window::noise, rounding); // grain on the panel as well, with the shader background the blur would smooth the window's away
}

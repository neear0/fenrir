#include "../ui.h"

void celosia::widgets::add_rect_filled_half_rounded(ImDrawList* drawlist, const ImVec2& min, const ImVec2& max, const ImColor& clr, float rounding) {
	drawlist->AddRectFilled(min, max, clr, rounding);
	drawlist->AddRectFilled(ImVec2(min.x, min.y + (max.y - min.y) / 2), max, clr, 0);
}

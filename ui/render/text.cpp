#include "../ui.h"

// text measured and drawn with ImGui's fonts into any draw list. font nullptr is the current font. ImFont::RenderText
// floors where a line starts, so text lands on whole pixels wherever it's put

static float font_size(ImFont* font) {
	return font ? font->FontSize : celosia::core::font_size();
}

const char* celosia::render::text::label_end(const char* label) {
	const char* end = label;
	while (*end && !(end[0] == '#' && end[1] == '#'))
		end++;
	return end;
}

ImVec2 celosia::render::text::size(const char* text, const char* text_end, ImFont* font) {
	if (text == text_end || !*text)
		return ImVec2(0.f, font_size(font));
	ImVec2 size = (font ? font : core::font())->CalcTextSizeA(font_size(font), FLT_MAX, 0.f, text, text_end);
	size.x = IM_TRUNC(size.x + 0.99999f); // rounded up to whole pixels, the room it takes
	return size;
}

void celosia::render::text::draw(ImDrawList* drawlist, ImVec2 pos, ImU32 color, const char* text, const char* text_end, ImFont* font) {
	if (text == text_end || !*text)
		return;
	drawlist->AddText(font, font ? font->FontSize : 0.f, pos, color, text, text_end); // font nullptr: the draw list's, the current one
}

void celosia::render::text::draw_aligned(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImVec2 align, ImU32 color, const char* text, const char* text_end, const ImVec2* known_size, const ImRect* clip) {
	if (text == text_end || !*text)
		return;
	const ImVec2 text_size = known_size ? *known_size : size(text, text_end);
	const ImVec2 clip_min = clip ? clip->Min : min, clip_max = clip ? clip->Max : max;
	ImVec2 pos = min;
	// clipped on the cpu only when it doesn't fit, a draw without a clip rect is cheaper
	bool clipped = pos.x + text_size.x >= clip_max.x || pos.y + text_size.y >= clip_max.y;
	if (clip)
		clipped |= pos.x < clip_min.x || pos.y < clip_min.y;
	if (align.x > 0.f) pos.x = ImMax(pos.x, pos.x + (max.x - pos.x - text_size.x) * align.x);
	if (align.y > 0.f) pos.y = ImMax(pos.y, pos.y + (max.y - pos.y - text_size.y) * align.y);
	const ImVec4 clip_rect(clip_min.x, clip_min.y, clip_max.x, clip_max.y);
	drawlist->AddText(nullptr, 0.f, pos, color, text, text_end, 0.f, clipped ? &clip_rect : nullptr);
}

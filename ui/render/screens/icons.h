#pragma once

// fenrir's icons, drawn with draw list lines and shapes so they stay sharp at any scale and take any color. size is the
// box they fit in and thickness their stroke, in the same units as the draw list
namespace fenrir::icons {
	enum class e_icon { home, recover, images, erase, reports, settings, lock, minimize, close };

	void draw(ImDrawList* drawlist, e_icon icon, ImVec2 center, float size, ImU32 color, float thickness);
	void fehu(ImDrawList* drawlist, ImVec2 center, float size, ImU32 color, float thickness); // the rune for f, fenrir's mark
}

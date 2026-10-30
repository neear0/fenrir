#include "../ui.h"

static void fade(ImDrawList* drawlist, int vtx_begin, int vtx_end, float alpha) { // scales the alpha of what's already in the draw list
	for (int i = vtx_begin; i < vtx_end; i++) {
		ImU32& col = drawlist->VtxBuffer[i].col;
		const ImU32 a = (ImU32)(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
		col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}
}

void celosia::animations::functions::tab_switch() {
	static std::string tab_old;
	static float progress = 0.f; // 0 shows the tab, 1 has it covered and moved down all the way
	static bool fading_out = false;
	static const ImGuiID spacing = animations::key("groupbox_active_spacing");

	if (tab_old.empty()) // the first tab is shown at once
		tab_old = ui::tab_active;

	if (tab_old != ui::tab_active) {
		fading_out = true; // from wherever it is, so picking another tab mid-switch doesn't jump
		tab_old = ui::tab_active;

		debug::log("tab switch animation activated");
	}

	if (fading_out) {
		progress = ImMin(progress + delta_time / style::frame::tab_switch_out, 1.f);
		if (progress >= 1.f) {
			fading_out = false;
			ui::tab_current = ui::tab_active; // the new groupboxes appear while covered
		}
	}
	else if (progress > 0.f)
		progress = ImMax(progress - delta_time / style::frame::tab_switch_in, 0.f);

	// eased so it starts and stops gently instead of at full speed
	const float amount = progress * progress * (3.f - 2.f * progress);
	animations::snap(spacing, amount * style::frame::tab_switch_animation_max);

	// the tab fades instead of being covered: a cover in the background color would flash opaque over the os blur.
	// the effect shaders multiply by the vertex alpha too, so the gradients, glass and grain fade along with the rest
	if (amount > 0.f && render::tab_content.drawlist) {
		const float alpha = 1.f - amount;
		fade(render::tab_content.drawlist, render::tab_content.vtx_begin, render::tab_content.vtx_end, alpha);
		for (ImDrawList* child : render::tab_content.children)
			fade(child, 0, child->VtxBuffer.Size, alpha);
	}
}

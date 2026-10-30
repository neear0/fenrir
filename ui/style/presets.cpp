#include "../ui.h"

void celosia::style::presets::set_glass(const t_glass& preset) {
	window::glass_blur = preset.blur * scale;
	window::glass_opacity = preset.opacity;
}

int celosia::style::presets::glass_current() {
	for (int i = 0; i < IM_ARRAYSIZE(glass); i++) {
		const bool blur = fabsf(window::glass_blur - glass[i].blur * scale) < 0.5f; // the slider shows whole pixels
		// the opacity only shows with the blur background, with the shader one it's hidden and doesn't count
		const bool opacity = window::background != window::blur || fabsf(window::glass_opacity - glass[i].opacity) < 0.005f;
		if (blur && opacity)
			return i;
	}
	return -1;
}

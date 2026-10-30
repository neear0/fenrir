#include "../ui.h"

int celosia::render::tabs(std::initializer_list<const char*> labels, e_tabs_layout layout) {
	if (labels.size() == 0)
		return -1;
	if (ui::tab_active.empty()) // nothing picked yet, the first tab starts open
		ui::tab_active = ui::tab_current = *labels.begin();

	const float available = core::available().x - style::general::padding.x; // the same gap on the right as on the left
	const float gap = style::general::spacing.y;
	const float count = (float)labels.size();
	int active = -1, index = 0;
	for (const char* label : labels) {
		ImVec2 size;
		if (layout == e_tabs_layout::vertical)
			size = ImVec2(available, 0.f);
		else {
			size = ImVec2(IM_TRUNC((available - gap * (count - 1.f)) / count), 0.f);
			if (index > 0)
				core::same_line(gap);
		}

		celosia::widgets::tab_button(label, size);
		if (ui::tab_active == label)
			active = index;
		index++;
	}
	return active;
}

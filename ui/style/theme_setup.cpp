#include "../ui.h"

static celosia::style::themes::t_theme target; // what set() was last given, active eases towards it

void celosia::style::themes::initialize() {
	dark.name = "dark";
	dark.text = ImColor(255, 255, 255, 255);
	dark.background = ImColor(12, 12, 12, 180);
	dark.background_darker = ImColor(10, 10, 10, 200);
	dark.idle = ImColor(18, 18, 18, 255);
	dark.hovered = ImColor(64, 64, 64, 255);
	dark.held = ImColor(16, 16, 16, 255);
	dark.active = ImColor(28, 28, 28, 255);
	dark.frame_border = ImColor(0, 0, 0, 0);
	dark.frame_fill = ImColor(16, 16, 16, 255);

	light.name = "light";
	light.text = ImColor(18, 18, 18, 255);
	light.background = ImColor(242, 242, 245, 190);
	light.background_darker = ImColor(226, 226, 230, 210);
	light.idle = ImColor(252, 252, 252, 255);
	light.hovered = ImColor(205, 205, 212, 255);
	light.held = ImColor(190, 190, 198, 255);
	light.active = ImColor(235, 235, 240, 255);
	light.frame_border = ImColor(0, 0, 0, 0);
	light.frame_fill = ImColor(236, 236, 240, 255);
	light.glass_floor = 0.6f;
}

void celosia::style::themes::set(const t_theme& theme, bool instant) {
	target = theme;
	active.name = theme.name;
	if (instant)
		active = theme;
}

void celosia::style::themes::update() {
	static constexpr ImColor t_theme::* colors[] = { // the members that ease, the rest switch at once
		&t_theme::background, &t_theme::background_darker, &t_theme::frame_border, &t_theme::frame_fill,
		&t_theme::idle, &t_theme::held, &t_theme::hovered, &t_theme::active, &t_theme::text,
	};
	const float t = 1.f - expf(-animations::delta_time * lerp_speed); // the same share of the way each second, whatever the frame rate
	for (ImColor t_theme::* color : colors) {
		const ImVec4 from = (active.*color).Value, to = (target.*color).Value;
		(active.*color).Value = ImLerp(from, to, t);
	}
	active.glass_floor = ImLerp(active.glass_floor, target.glass_floor, t);
	apply();
}

float celosia::style::window::glass_cover() {
	return ImLerp(themes::active.glass_floor, 1.f, glass_opacity);
}

ImColor celosia::style::window::background_color() {
	ImColor color = themes::active.background;
	if (background == blur) // the theme's alpha alone hides most of the blurred desktop
		color.Value.w *= glass_cover();
	return color;
}

void celosia::style::keep_in_limits() {
	window::glass_blur = ImClamp(window::glass_blur, limits::glass_blur_min * scale, limits::glass_blur_max * scale);
	window::glass_opacity = ImClamp(window::glass_opacity, limits::glass_opacity_min, 1.f);
	window::noise = ImClamp(window::noise, 0.f, limits::noise_max);

	ImVec4& color = general::main_color.Value;
	float h, s, v;
	ImGui::ColorConvertRGBtoHSV(color.x, color.y, color.z, h, s, v);
	if (s < limits::saturation_min || v < limits::value_min) // a color already in limits isn't round tripped, it stays exactly as set
		ImGui::ColorConvertHSVtoRGB(h, ImMax(s, limits::saturation_min), ImMax(v, limits::value_min), color.x, color.y, color.z);
	color.w = 1.f; // never see-through, the titles and checkmarks would fade into what's behind them
}

void celosia::style::apply() {
	keep_in_limits();
	general::accent = themes::readable(general::main_color, themes::active.background, limits::accent_contrast_min); // follows the theme as it eases
}

void celosia::style::set_scale(float new_scale) {
	if (new_scale <= 0.f || new_scale == scale)
		return;
	const float factor = new_scale / scale;
	scale = new_scale;
	const auto scale_int = [factor](int& value) { value = (int)IM_ROUND(value * factor); };

	scale_int(general::rounding);
	scale_int(general::rounding_window);
	scale_int(general::rounding_popup);
	general::padding *= factor;
	general::spacing *= factor;
	general::inner_spacing = ImTrunc(general::inner_spacing * factor);
	general::popup_margin = ImTrunc(general::popup_margin * factor);
	scrollbar::width *= factor;
	scrollbar::rounding = ImTrunc(scrollbar::rounding * factor);
	scrollbar::grab_min = ImTrunc(scrollbar::grab_min * factor);
	window::glass_blur *= factor;
	titlebar::height *= factor;
	scale_int(titlebar::padding);
	sidebar::width *= factor;
	frame::size_padding *= factor;
	frame::border_size *= factor;
	frame::tab_switch_animation_max *= factor;
	groupbox::height *= factor;
	scale_int(groupbox::padding);
	ui::size = ImVec2(IM_ROUND(ui::size.x * factor), IM_ROUND(ui::size.y * factor));
	apply();
}

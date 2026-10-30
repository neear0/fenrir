#include "../ui.h"

ImColor celosia::style::themes::rgb_accent(ImColor clr) { // a neighbouring hue, a little more saturated, for gradients from the main color
	float h, s, v;
	ImGui::ColorConvertRGBtoHSV(clr.Value.x, clr.Value.y, clr.Value.z, h, s, v);
	h = ImFmod(h + 0.02f, 1.f);
	s = ImMin(s * 1.3f, 1.f);
	return ImColor::HSV(h, s, v, clr.Value.w);
}

ImColor celosia::style::themes::rgb_intensity(ImColor clr, float percentage) {
	return ImColor(clr.Value.x * percentage, clr.Value.y * percentage, clr.Value.z * percentage, clr.Value.w);
}

static float linear(float channel) { // srgb to linear light
	return channel <= 0.04045f ? channel / 12.92f : powf((channel + 0.055f) / 1.055f, 2.4f);
}

float celosia::style::themes::luminance(ImColor clr) { // how bright a color looks, 0 black to 1 white
	return 0.2126f * linear(clr.Value.x) + 0.7152f * linear(clr.Value.y) + 0.0722f * linear(clr.Value.z);
}

float celosia::style::themes::contrast(ImColor a, ImColor b) { // 1 for the same brightness up to 21 for black on white
	const float la = luminance(a), lb = luminance(b);
	return (ImMax(la, lb) + 0.05f) / (ImMin(la, lb) + 0.05f);
}

ImColor celosia::style::themes::readable(ImColor clr, ImColor background, float contrast_min) {
	if (contrast(clr, background) >= contrast_min)
		return clr;
	// lighter towards white on a dark background, darker towards black on a light one, as little as gets there
	const ImVec4 target = luminance(background) < 0.18f ? ImVec4(1, 1, 1, clr.Value.w) : ImVec4(0, 0, 0, clr.Value.w);
	float low = 0.f, high = 1.f;
	for (int i = 0; i < 10; i++) { // all the way to the target always gets there, halving keeps the least mix found that does, to a thousandth
		const float mid = (low + high) * 0.5f;
		if (contrast(ImLerp(clr.Value, target, mid), background) >= contrast_min)
			high = mid;
		else
			low = mid;
	}
	return ImLerp(clr.Value, target, high);
}

ImColor celosia::style::themes::text_on(ImColor fill) { // dark or white, whichever stands out more from fill
	const ImColor dark_text(0.15f, 0.15f, 0.15f), white_text(1.f, 1.f, 1.f);
	return contrast(fill, dark_text) > contrast(fill, white_text) ? dark_text : white_text;
}

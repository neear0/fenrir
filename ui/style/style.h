#pragma once

namespace celosia::style {
	// how big the ui is drawn, 1 at 96 dpi. platform::create() sets it from the monitor the window opens on and set_scale()
	// scales the sizes in here along, they're in pixels at scale 1 until then
	inline float scale = 1.f;

	namespace general {
		inline ImColor main_color = { 189, 122, 255,255 }; // the one the user picks
		inline ImColor accent = main_color; // main_color as it's drawn: lighter or darker where the theme needs it to stand out, apply() works it out
		inline int rounding = 4;
		inline int rounding_window = 0;
		inline int rounding_popup = 0;
		inline float scroll_speed = 15.f;
		inline float scroll_multiplier = 2.f;
		inline ImVec2 padding = {10, 10};
		inline ImVec2 spacing = {0, 4}; // between items
		inline ImVec2 inner_spacing = {4, 4}; // inside an item, e.g. between a frame and its label
		inline float popup_margin = 3.f; // kept between a popup and the window's edges
	}
	namespace scrollbar {
		inline float width = 4.f;
		inline float rounding = 9.f;  // of the grab
		inline float grab_min = 12.f; // the shortest the grab gets
	}
	namespace window {
		enum e_background { blur, shader };
		inline e_background background = blur; // blur: the os blurs the desktop behind the window (windows, kwin on x11), shader: effects::background with glass panels
		inline float noise = 0.04f; // film grain (effects::noise) over the background and panels, how strong it is, 0 turns it off
		inline float glass_blur = 12.f; // radius in pixels of the frosted glass (effects::blur): the popups, and the panels over the shader background
		inline float glass_opacity = 0.4f; // with the blur background: how much the window's background and panels cover the blurred desktop, see glass_cover()
		float glass_cover(); // glass_opacity from the active theme's glass_floor up to 1, what the window's background and panels are multiplied by
		ImColor background_color(); // the active theme's background as the window draws it, see-through by glass_cover() over the os blur
	}
	// how far the values above that a user can change may go: far enough to make the window their own, not so far it falls
	// apart. keep_in_limits() holds them to these whatever set them, the demo's sliders and color picker only reach this far
	namespace limits {
		inline float saturation_min = 0.3f; // of the main color, greyer and the accent no longer stands out from the theme
		inline float value_min = 0.5f; // of the main color, darker and the groupbox titles, the checkmarks and the logo disappear
		inline float glass_blur_min = 6.f; // in pixels at scale 1, less and the glass is only a tint with what's behind it sharp
		inline float glass_blur_max = 32.f;
		inline float glass_opacity_min = 0.3f; // less and the text is lost against a bright desktop, or a dark one with the light theme
		inline float noise_max = 0.1f;
		inline float accent_contrast_min = 3.f; // between the accent and the theme's background, the least for parts of a ui to be told apart
	}
	// ready made choices for the values above, all inside limits: what a user picks from first, the sliders and the
	// color picker are there for going further
	namespace presets {
		inline ImColor accents[] = {
			{ 189, 122, 255, 255 }, // violet, the default
			{ 92, 140, 255, 255 },  // blue
			{ 38, 198, 180, 255 },  // teal
			{ 110, 200, 90, 255 },  // green
			{ 255, 176, 64, 255 },  // amber
			{ 255, 96, 96, 255 },   // coral
			{ 255, 110, 190, 255 }, // pink
		};
		struct t_glass {
			const char* name;
			float blur;    // window::glass_blur in pixels at scale 1
			float opacity; // window::glass_opacity
		};
		inline t_glass glass[] = { { "Clear", 6.f, 0.3f }, { "Frosted", 12.f, 0.4f }, { "Solid", 24.f, 0.85f } };
		void set_glass(const t_glass& preset);
		int glass_current(); // the glass preset the window is set to, -1 once the sliders moved it off them all
		inline float noise = 0.04f; // window::noise when the film grain is on
	}
	namespace titlebar {
		inline float height = 45;
		inline int padding = 2;
	}
	namespace sidebar {
		inline float width = 165;
	}
	namespace frame {
		inline ImVec2 size_padding = {10,10};
		inline float border_size = 2.f;

		inline float tab_switch_animation_max = 16; // how far the groupboxes move down while the tab switches
		inline float tab_switch_out = 0.12f; // seconds the old tab takes to fade out
		inline float tab_switch_in = 0.3f; // seconds the new tab takes to fade in
	}
	namespace groupbox {
		inline float height = 40; // of the title, the least it can be, see render::groupbox::title_height()
		inline int padding = titlebar::padding * 4;
		enum e_title_style { full, minimal }; // full: a gradient bar, minimal: a plain bar with a gradient underline
		inline e_title_style title_style = full; // not part of a theme, switching the theme keeps it
	}

	namespace themes {
		struct t_theme {
			std::string name;

			ImColor background;
			ImColor background_darker;

			ImColor frame_border;
			ImColor frame_fill;

			ImColor idle;
			ImColor held;
			ImColor hovered;
			ImColor active;

			ImColor text;

			float glass_floor = 0.f; // with the blur background: the least window::glass_cover() goes, a light theme over a dark desktop is grey otherwise
		};

		inline t_theme dark;
		inline t_theme light;

		inline t_theme active;

		void initialize();
		inline float lerp_speed = 8.f; // how fast set() eases into a new theme
		void set(const t_theme& theme, bool instant = false); // eases active into theme, or switches at once
		void update(); // eases active a step towards the theme given to set() and applies it, ui::begin() calls it every frame

		ImColor rgb_accent(ImColor clr);
		ImColor rgb_intensity(ImColor clr, float percentage);
		float luminance(ImColor clr);
		float contrast(ImColor a, ImColor b);
		ImColor readable(ImColor clr, ImColor background, float contrast_min); // clr mixed towards white or black just enough to reach contrast_min against background
		ImColor text_on(ImColor fill); // dark or white text, whichever stands out more from fill
	}

	void keep_in_limits(); // brings the main color, glass and noise back into limits, an opaque main color too. apply() calls it
	void apply(); // keeps the values in limits and works out the accent, every frame
	void set_scale(float new_scale); // scales the sizes above and ui::size from the current scale to new_scale
}
#pragma once

// our widgets, built on core (core/core.h)
namespace celosia::widgets {
	// widgets
	bool button(const char* label, const ImVec2& size = ImVec2(0, 0)); // size 0 fits the text, true when it was clicked
	bool checkbox(const char* label, bool* v); // a box with a check mark that eases in and out, true when it was flipped
	bool combo(const char* label, int* current, const char* const items[], int count, int max_items = -1); // a frame opening a popup to pick one of items from, max_items rows before it scrolls (-1 is 8). true when the pick changed
	bool tab_button(const char* label, const ImVec2& size = ImVec2(0, 0)); // render::tabs() lays these out, size 0 is the sidebar's width and the text's height
	bool checkbox_map(const char* label); // checkbox() on variables::config::bools[label]
	void image(const char* name, ImVec2 size = ImVec2(0, 0), float rounding = 0.f); // a resources::images image as an item, size 0 is its own size
	bool color_edit(const char* label, float col[4], bool alpha = true, float saturation_min = 0.f, float value_min = 0.f); // swatch that opens a color picker popup, true when the color changed
	bool color_picker(const char* label, float col[4], bool alpha = true, float saturation_min = 0.f, float value_min = 0.f); // saturation/value square, hue bar and alpha bar. the square only spans saturations and values from the mins up
	bool slider_float(const char* label, float* v, float min, float max, const char* format = "%.1f"); // dragged along a track, true when the value changed. format shows the value
	bool segmented(const char* label, int* current, const char* const items[], int count); // side by side choices in one frame, the picked one in the accent, current -1 picks none. true when one was clicked
	bool swatches(const char* label, float col[4], const ImColor* colors, int count); // a row of round color choices, sets col to the clicked one and rings the one col is, true when it changed
	bool toggle(const char* label, bool* v); // an on/off switch, true when it was flipped
	void text(const char* text); // a line of text as an item

	// drawing
	void add_rect_filled_half_rounded(ImDrawList* drawlist, const ImVec2& min, const ImVec2& max, const ImColor& clr, float rounding);

	// item frames, imgui/functions/. id is the item's, the animated ones ease their color per item
	void render_frame_border_color(ImVec2 p_min, ImVec2 p_max, ImColor clr, float rounding = 0.0f);
	void render_frame_border_animated(ImGuiID id, ImVec2 p_min, ImVec2 p_max, ImColor fill_col, float rounding = 0.0f);
	void render_frame_animated(ImGuiID id, ImVec2 p_min, ImVec2 p_max, ImColor fill_col, float rounding = 0.0f);
}

// the surfaces widgets go on, core's surfaces (core/surfaces.cpp)
namespace celosia::window {
	void begin(const char* name, ImVec2 pos, ImVec2 size); // the window the ui fills: no title bar, can't be moved or resized by itself
	void end();
}
namespace celosia::scroll_area {
	void begin(ImGuiID id, ImVec2 size); // a region at the cursor that scrolls smoothly when its content doesn't fit
	void begin(const char* name, ImVec2 size);
	void end();
}
namespace celosia::popup {
	void open(ImGuiID id);
	bool is_open(ImGuiID id);
	// while popup id is open: a frosted window over the ui under anchor (above it when there's no room below), padding
	// inside it, at least min_width wide and at most max_height tall, sized to what's in it. end() only when it returns true
	bool begin(ImGuiID id, const ImRect& anchor, ImVec2 padding, float min_width = 0.f, float max_height = FLT_MAX);
	void end();
	void close_current(); // the popup being drawn, e.g. once one of its items was picked
}

#pragma once

namespace celosia::render {
	struct t_tab_content { // what the open tab drew this frame, the tab switch fades it out and in
		ImDrawList* drawlist = nullptr; // the window's, from vtx_begin to vtx_end: the groupbox panels and titles
		int vtx_begin = 0, vtx_end = 0;
		ImVector<ImDrawList*> children; // the groupboxes' child windows, all of them
	};
	inline t_tab_content tab_content; // cleared every frame, filled by main_window() and groupbox::begin()

	void main_window(); // the demo window, see main_window.cpp
	void titlebar();
	void sidebar();
	void panel(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor color, float rounding); // glass with the shader background, a flat fill otherwise

	// text in any draw list, font nullptr is the current font. text_end nullptr goes to the end of the string, a label's
	// shown part ends at label_end(), what's after "##" only makes its id
	namespace text {
		const char* label_end(const char* label);
		ImVec2 size(const char* text, const char* text_end = nullptr, ImFont* font = nullptr); // width rounded up to whole pixels
		void draw(ImDrawList* drawlist, ImVec2 pos, ImU32 color, const char* text, const char* text_end = nullptr, ImFont* font = nullptr);
		// in the current font, placed in min..max by align (0 left/top, 1 right/bottom) and cut off at clip (min..max when nullptr)
		void draw_aligned(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImVec2 align, ImU32 color, const char* text, const char* text_end = nullptr, const ImVec2* known_size = nullptr, const ImRect* clip = nullptr);
	}

	enum class e_tabs_layout { vertical, horizontal };
	// tab buttons that place and size themselves in the space left in the window: one under the other at full width, or side by side
	// sharing the width. returns the index of the active tab, the first one starts active when none is
	int tabs(std::initializer_list<const char*> labels, e_tabs_layout layout = e_tabs_layout::vertical);

	namespace groupbox {
		enum e_group_layout { fullscreen, two, four };
		inline int index = 0; // position of the next groupbox, reset every frame
		float title_height(const char* description = ""); // of the title at the top, grows when the text needs it
		void body(ImDrawList* drawlist, const ImVec2 offset, const ImVec2 offset_max, const char* title = "", const char* description = "");
		// layout: fullscreen fills the space right of the sidebar, two stack in it, four make a 2x2 grid. groupbox::index picks the cell
		void begin(const char* title, const char* description = "", e_group_layout layout = e_group_layout::two);
		void end();
	}
}
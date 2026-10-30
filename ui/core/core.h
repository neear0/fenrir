#pragma once

// what the widgets are built on: ids, the mouse over items, the layout of a surface and the surfaces themselves (the
// window, scroll areas in it and popups over it, see widgets.h), each with its own draw list. ui::begin() starts a frame
// with new_frame(), ui::end() finishes it with end_frame() and hands draw_data() to the platform.
// positions are in units of ui::size from the window's top left, like the draw lists
namespace celosia::core {
	void new_frame();        // after input::new_frame(): what the mouse is over
	int frame();             // counts up with new_frame()
	void end_frame();        // popups clicked away from close, the keyboard moves, the draw lists go into draw_data()
	ImDrawData* draw_data(); // this frame's, in the order the surfaces were drawn

	// an item's id is its label hashed with the ids pushed in the surface it's in, "##" and what follows included.
	// every surface starts from its own id, so the same label in two surfaces makes two ids
	ImGuiID id(const char* label);
	ImGuiID id(int n);
	void push_id(const char* label);
	void push_id(int n);
	void pop_id();

	ImFont* font();       // what text is drawn in when no font is given, the "default" font
	float font_size();
	float frame_height(); // font_size() with the frame padding above and below it, how tall a frame is

	// layout of the surface being filled. the cursor is where the next item goes, layout() takes the room of an item there
	// and moves the cursor to the next line, same_line() puts the next item right of the last one instead
	ImDrawList* drawlist();
	ImVec2 cursor();
	void set_cursor(ImVec2 local); // from the surface's top left, scrolled along with its content
	void same_line(float spacing = -1.f); // spacing -1 is style::general::spacing.x
	ImVec2 available();  // from the cursor to the right and bottom of the surface's content
	float item_width();  // how wide a frame is by default, 65% of the surface's width
	float line_end();    // where the surface's content ends on the right
	float text_offset(); // how far under the cursor the text of the items on this line sits, text after them lines up with it
	ImVec2 item_size(ImVec2 size, float default_w, float default_h); // a 0 takes the default, a negative is that far from the content's right or bottom
	// text_baseline: how far down the item its text sits, for lining up the text of the items on a line, -1 when it has none
	void layout(const ImVec2& size, float text_baseline = -1.f);
	void layout(const ImRect& bb, float text_baseline = -1.f);

	// items. add() says where an item is, false when it can't be seen: it's then neither drawn nor can it be used. id 0 for
	// one that only shows something. button() is the mouse on an item: hovered while the mouse is over it (and no other item is
	// held), held from a click on it until the button goes up, true the frame it goes up over it, or enter or space was pressed
	// on it. item_no_focus: the keyboard skips it (a scrollbar, a drag area, a row of choices whose choices take it)
	enum e_item_flags { item_none = 0, item_no_focus = 1 << 0 };
	bool add(const ImRect& bb, ImGuiID id, int flags = item_none);
	bool hoverable(const ImRect& bb, ImGuiID id);
	bool button(const ImRect& bb, ImGuiID id, bool* hovered = nullptr, bool* held = nullptr, int flags = item_none);
	ImGuiID active();       // the item being held, 0 when there's none
	bool activated(ImGuiID id); // it was clicked this frame
	void hover(ImGuiID id); // hovered whatever button() said, for one dragged outside of itself (the scrollbar)
	ImGuiStorage* storage(); // values items keep between frames, by id

	// the keyboard: tab and shift+tab go through the items in the order they're drawn, the arrows to the nearest one that way,
	// enter and space press the one it's on, escape closes the popup it's in and goes back to what opened it. it scrolls what
	// it moves to into view, a ring in the accent shows where it is. a click puts it on the item clicked and hides the ring
	inline bool keyboard_nav = true; // off, the keys do nothing but close popups, the demo's Appearance tab switches it
	bool focused(ImGuiID id);            // the keyboard is on it, with the ring showing
	bool focus_key(ImGuiID id, int key); // focused(id) and key was pressed, which then doesn't move the keyboard: a slider takes left and right
	void default_focus(ImGuiID id);      // where the keyboard starts in a popup opened with it, the combo's picked item

	// surfaces, what widgets.h's window, scroll_area and popup are made of
	struct t_surface {
		enum e_kind { window, scroll_area, popup };
		e_kind kind = window;
		ImGuiID id = 0;
		t_surface* parent = nullptr;       // what a scroll area is in
		std::vector<t_surface*> children;  // the scroll areas in it this frame, in the order they were begun
		std::vector<ImGuiID> id_stack;
		ImDrawList drawlist;
		int frame = -1;      // the last frame it was begun in
		bool shown = false;  // drawn this frame. a popup isn't on the frame it opens, its size comes from what's in it
		double last_used = 0.0;

		ImVec2 pos, size, padding;
		ImRect inner;        // pos to pos + size without the scrollbar
		ImRect clip;         // what items are clipped to
		ImRect hover_rect;   // where it can be hovered, cut off where what it's in ends
		ImRect work;         // where its content goes
		float item_width = 0.f;

		// scrolling, vertical only. content is what last frame's items took up
		ImVec2 content;
		float scroll = 0.f, scroll_max = 0.f;
		float scroll_target = 0.f, scroll_eased = 0.f; // where it's heading and where it is on the way, see scroll_area.cpp
		bool scrollbar = false;

		// the layout's cursor
		ImVec2 cursor, cursor_start, cursor_max, cursor_line_end; // cursor_line_end: right of the last item, at its line's top
		float line_height = 0.f, line_height_last = 0.f;
		float line_text_offset = 0.f, line_text_offset_last = 0.f;
		bool same_line = false;

		t_surface();
	};
	t_surface* surface();          // the one being filled
	t_surface* find(ImGuiID id);   // a surface by id, null when there's none
	t_surface* begin_surface(ImGuiID id, t_surface::e_kind kind, ImVec2 pos, ImVec2 size, ImVec2 padding);
	void end_surface();
	void scroll_by(t_surface* surface, float notches); // the mouse wheel, eased, see scroll_area.cpp
	void scrollbar(t_surface* surface);                 // draws it and lets it be dragged, see scroll_area.cpp

	// popups open over the window in a stack, a popup opened from inside another one goes over it
	void open_popup(ImGuiID id);
	bool popup_open(ImGuiID id);     // at the level being drawn
	void close_popup();              // the one being drawn and any over it
	void close_popups(int level);    // from that level up
	int popup_level();               // how many popups are being drawn, 0 outside of them
	void begin_popup();              // drawing the popup open at popup_level(), popup_open() has to be true for it
	void end_popup();
}

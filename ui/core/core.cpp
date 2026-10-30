#include "../ui.h"

// the state behind core.h. the layout follows the rules ImGui's had (where a line ends, how text on a line lines up,
// what gets rounded to whole pixels) so items land where they did

using celosia::core::t_surface;

static ImDrawListSharedData shared; // what every draw list needs from the font atlas
static ImDrawData data;
static std::unordered_map<ImGuiID, std::unique_ptr<t_surface>> surfaces;
static std::vector<t_surface*> stack;              // surfaces being filled, the current one last
static std::vector<t_surface*> roots, roots_last;  // windows and popups in the order they were begun, this frame and last
static int frame_count = 0;
static ImFont* default_font = nullptr;
static ImGuiStorage storage_values;

static ImGuiID active_id = 0;    // the item being held
static bool active_alive = false; // it was there this frame
static int active_frame = -1;     // the frame it was clicked
static ImGuiID hovered_id = 0;   // the first item to be hovered this frame, the ones after it under the mouse aren't
static t_surface* hovered = nullptr;      // the surface under the mouse, from where they were last frame
static ImGuiID wheel_target = 0;          // the surface the mouse wheel scrolls
static const ImGuiID void_id = ~0u;       // held: a click that landed on no item, nothing hovers until it's let go

struct t_popup {
	ImGuiID id;
	int open_frame;
	int begin_frame = -1;
	ImGuiID opener = 0;    // the item it was opened from, the keyboard goes back to it when it closes
	bool keyboard = false; // opened with the keyboard, which then goes into it
};
static std::vector<t_popup> popups; // open ones, the top one last
static int popup_depth = 0;         // popups being drawn

struct t_focusable {
	ImGuiID id;
	ImRect rect;
	t_surface* surface;
	int level; // the popup level it's in, 0 outside of popups
};
static std::vector<t_focusable> focusables; // this frame's items the keyboard can go to, in the order they were drawn
static ImGuiID focus_id = 0;       // the item the keyboard is on
static int focus_level = 0;        // the popup level it's in
static bool focus_visible = false; // a key put it there, the ring shows
static ImGuiID focus_default = 0;  // default_focus() this frame
static int focus_default_level = 0;
static ImGuiID activate_id = 0;    // enter or space was pressed on it, its button() is pressed this frame
static ImGuiID pressed_id = 0;     // what button() said was pressed this frame
static bool pressed_by_key = false;
static std::array<bool, 4> arrows_taken{}; // focus_key() took them this frame, they don't move the keyboard

celosia::core::t_surface::t_surface() : drawlist(&shared) {}

static t_surface* surface_under(ImVec2 mouse) { // the deepest surface under the mouse, popups blocking the window
	t_surface* root = nullptr;
	if (!popups.empty()) { // only a popup can be hovered then, clicking anywhere else closes them
		for (int i = (int)popups.size() - 1; i >= 0 && !root; i--) {
			t_surface* popup = celosia::core::find(popups[i].id);
			if (popup && popup->shown && popup->frame == frame_count - 1 && popup->hover_rect.Contains(mouse))
				root = popup;
		}
	}
	else {
		for (int i = (int)roots_last.size() - 1; i >= 0 && !root; i--)
			if (roots_last[i]->kind == t_surface::window && roots_last[i]->hover_rect.Contains(mouse))
				root = roots_last[i];
	}
	for (t_surface* s = root; s; ) { // into the scroll areas, the last one begun is on top
		t_surface* child = nullptr;
		for (int i = (int)s->children.size() - 1; i >= 0 && !child; i--)
			if (s->children[i]->hover_rect.Contains(mouse))
				child = s->children[i];
		if (!child)
			return s;
		s = child;
	}
	return nullptr;
}

void celosia::core::new_frame() {
	frame_count++;
	default_font = resources::fonts::get("default");
	ImFontAtlas* atlas = default_font->ContainerAtlas;
	shared.Font = default_font;
	shared.FontSize = font_size();
	shared.TexUvWhitePixel = atlas->TexUvWhitePixel;
	shared.TexUvLines = atlas->TexUvLines;
	shared.ClipRectFullscreen = ImVec4(0.f, 0.f, ui::display_size.x, ui::display_size.y);
	shared.CurveTessellationTol = 1.25f;
	shared.SetCircleTessellationMaxError(0.3f);
	shared.InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
	if (!(atlas->Flags & ImFontAtlasFlags_NoBakedLines))
		shared.InitialFlags |= ImDrawListFlags_AntiAliasedLinesUseTex;
	if (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset)
		shared.InitialFlags |= ImDrawListFlags_AllowVtxOffset;

	// the held item lets go once it's gone (its popup closed, its tab switched), or for a click on no item, with the button
	if (active_id == void_id ? !input::mouse_down(input::left) : !active_alive)
		active_id = 0;
	active_alive = false;
	hovered_id = 0;

	hovered = surface_under(input::mouse_pos());
	wheel_target = 0;
	if (input::wheel() != 0.f) // the innermost one under the mouse that can scroll, the window doesn't
		for (t_surface* s = hovered; s && !wheel_target; s = s->parent)
			if (s->kind != t_surface::window && s->scroll_max > 0.f)
				wheel_target = s->id;

	focusables.clear();
	focus_default = 0;
	pressed_id = 0;
	arrows_taken.fill(false);
	roots.clear();
}

static int find_focusable(ImGuiID id) {
	for (int i = 0; i < (int)focusables.size(); i++)
		if (focusables[i].id == id)
			return i;
	return -1;
}

static int step(int from, int level, int direction) { // the next (1) or previous (-1) item in the level, around the end. from -1: the first or last
	const int count = (int)focusables.size();
	for (int k = 1; k <= count; k++) {
		const int i = from < 0 ? (direction > 0 ? k - 1 : count - k) : ((from + direction * k) % count + count) % count;
		if (focusables[i].level == level)
			return i;
	}
	return -1;
}

static float box_gap(float min_a, float max_a, float min_b, float max_b) { // from a to b on one axis, 0 when they overlap on it
	return min_b > max_a ? min_b - max_a : min_a > max_b ? max_b - min_a : 0.f;
}

static int nearest(int from, int arrow) { // the closest item that way, by the gap between them and then between their centers
	const t_focusable& current = focusables[from];
	const ImVec2 center = current.rect.GetCenter();
	int best = -1;
	float best_gap = FLT_MAX, best_centers = FLT_MAX;
	for (int i = 0; i < (int)focusables.size(); i++) {
		const t_focusable& f = focusables[i];
		if (i == from || f.level != current.level)
			continue;
		if (f.surface != current.surface && !f.rect.Overlaps(f.surface->clip)) // in another surface only what shows, in its own it scrolls into view
			continue;
		// which way it is: along the gap between the boxes, or between the centers when they overlap
		ImVec2 d(box_gap(current.rect.Min.x, current.rect.Max.x, f.rect.Min.x, f.rect.Max.x), box_gap(current.rect.Min.y, current.rect.Max.y, f.rect.Min.y, f.rect.Max.y));
		const float gap = ImFabs(d.x) + ImFabs(d.y);
		const ImVec2 c = f.rect.GetCenter() - center;
		const float centers = ImFabs(c.x) + ImFabs(c.y);
		if (d.x == 0.f && d.y == 0.f)
			d = c;
		const int way = ImFabs(d.x) > ImFabs(d.y) ? (d.x > 0.f ? celosia::keys::right : celosia::keys::left) : (d.y > 0.f ? celosia::keys::down : celosia::keys::up);
		if (way != arrow || (d.x == 0.f && d.y == 0.f))
			continue;
		if (gap < best_gap || (gap == best_gap && centers < best_centers)) {
			best_gap = gap;
			best_centers = centers;
			best = i;
		}
	}
	return best;
}

static void scroll_into_view(const t_focusable& f) { // with a little room around it
	t_surface* s = f.surface;
	if (s->kind == t_surface::window)
		return;
	const float room = celosia::style::general::spacing.y + 3.f * celosia::style::scale;
	float delta = 0.f;
	if (f.rect.Min.y - room < s->clip.Min.y)
		delta = f.rect.Min.y - room - s->clip.Min.y;
	else if (f.rect.Max.y + room > s->clip.Max.y)
		delta = f.rect.Max.y + room - s->clip.Max.y;
	if (delta != 0.f)
		s->scroll_target = ImClamp(s->scroll + delta, 0.f, s->scroll_max);
}

static void navigate() {
	using namespace celosia;
	activate_id = 0;
	int current = focus_id ? find_focusable(focus_id) : -1;
	if (current < 0) // its item is gone
		focus_id = 0;
	else
		focus_level = focusables[current].level;

	const bool window_focused = input::focused();
	if (window_focused && input::key_pressed(keys::escape, false)) { // out of the popup, back to what opened it
		if (!popups.empty())
			core::close_popups((int)popups.size() - 1);
		else
			focus_visible = false;
		return;
	}
	if (!core::keyboard_nav || !window_focused) {
		focus_visible = false;
		return;
	}

	const int level = (int)popups.size();
	int next = -1;
	if (level > 0 && popups.back().keyboard && focus_level != level) { // a popup opened with the keyboard starts on its default item, or its first
		const int fallback = focus_default && focus_default_level == level ? find_focusable(focus_default) : -1;
		next = fallback >= 0 ? fallback : step(-1, level, 1);
	}
	const bool here = current >= 0 && focusables[current].level == level;
	const bool modifier = input::key_down(keys::control) || input::key_down(keys::alt);
	if (next < 0 && input::key_pressed(keys::tab) && !modifier) // the first press only shows where it is
		next = here && !focus_visible ? current : step(here ? current : -1, level, input::key_down(keys::shift) ? -1 : 1);
	for (int arrow = keys::left; next < 0 && arrow <= keys::down; arrow++)
		if (input::key_pressed(arrow) && !arrows_taken[arrow - keys::left]) {
			const int found = here && focus_visible ? nearest(current, arrow) : -1;
			next = !here ? step(-1, level, 1) : found >= 0 ? found : current;
		}
	if (next >= 0) {
		focus_id = focusables[next].id;
		focus_level = level;
		focus_visible = true;
		scroll_into_view(focusables[next]);
		current = next;
	}

	if (focus_visible && current >= 0 && focusables[current].level == level && (input::key_pressed(keys::enter, false) || input::key_pressed(keys::space, false)))
		activate_id = focus_id;
}

static void draw_ring() { // around the item the keyboard is on, sliding from the last one, fading in and out
	using namespace celosia;
	static ImRect ring;
	static float alpha = 0.f;
	const int i = focus_id ? find_focusable(focus_id) : -1;
	const bool show = core::keyboard_nav && focus_visible && i >= 0;
	alpha = animations::functions::calc_smooth(alpha, show ? 1.f : 0.f, animations::color_speed);
	if (i < 0 || alpha < 0.01f || !focusables[i].surface->shown)
		return;

	const t_focusable& f = focusables[i];
	const float margin = 3.f * style::scale, thickness = 2.f * style::scale;
	ImRect goal = f.rect;
	goal.Expand(margin);
	if (alpha < 0.05f) // coming in, it starts where it goes
		ring = goal;
	const float speed = 18.f;
	ring = ImRect(animations::functions::calc_smooth(ring.Min.x, goal.Min.x, speed), animations::functions::calc_smooth(ring.Min.y, goal.Min.y, speed),
		animations::functions::calc_smooth(ring.Max.x, goal.Max.x, speed), animations::functions::calc_smooth(ring.Max.y, goal.Max.y, speed));

	ImDrawList* drawlist = &f.surface->drawlist; // over everything in its surface, a little past its edges
	ImRect clip = f.surface->clip;
	clip.Expand(margin + thickness);
	ImVec4 color = style::general::accent.Value;
	color.w *= alpha;
	drawlist->PushClipRect(clip.Min, clip.Max);
	drawlist->AddRect(ring.Min, ring.Max, ImColor(color), (float)style::general::rounding + margin, 0, thickness);
	drawlist->PopClipRect();
}

static void add_to_draw_data(t_surface* s) {
	data.AddDrawList(&s->drawlist);
	for (t_surface* child : s->children)
		add_to_draw_data(child);
}

void celosia::core::end_frame() {
	// a click on no item holds nothing, so what the mouse passes over until it's let go doesn't light up (dragging the titlebar)
	if (input::mouse_clicked(input::left) && active_id == 0 && hovered_id == 0)
		active_id = void_id;

	if (input::mouse_clicked(input::left) || input::mouse_clicked(input::right)) // the mouse took over, the ring goes
		focus_visible = false;

	// a click away from the popups closes them, the ones under the one it landed in stay
	if (!popups.empty() && (input::mouse_clicked(input::left) || input::mouse_clicked(input::right))) {
		t_surface* root = hovered;
		while (root && root->parent)
			root = root->parent;
		int keep = 0;
		for (int i = 0; i < (int)popups.size(); i++)
			if (root && popups[i].id == root->id)
				keep = i + 1;
		close_popups(keep);
	}
	for (int i = 0; i < (int)popups.size(); i++) // what opened a popup that wasn't drawn is gone
		if (popups[i].begin_frame != frame_count && popups[i].open_frame != frame_count) {
			close_popups(i);
			break;
		}
	navigate();
	draw_ring();

	data.Clear();
	data.Valid = true;
	data.DisplayPos = ImVec2(0.f, 0.f);
	data.DisplaySize = ui::display_size;
	data.FramebufferScale = ui::framebuffer_scale;
	for (t_surface* root : roots)
		if (root->shown)
			add_to_draw_data(root);
	roots_last = roots;

	for (auto& entry : surfaces) { // what hasn't been drawn for a while keeps its scroll, its draw list lets go of its memory
		t_surface* s = entry.second.get();
		if (s->frame < frame_count && s->drawlist.VtxBuffer.Capacity > 0 && ui::time - s->last_used > animations::forget_after)
			s->drawlist._ClearFreeMemory();
	}
}

ImDrawData* celosia::core::draw_data() { return &data; }
int celosia::core::frame() { return frame_count; }

static ImGuiID seed() { return stack.empty() ? 0 : stack.back()->id_stack.back(); }
ImGuiID celosia::core::id(const char* label) { return ImHashStr(label, 0, seed()); }
ImGuiID celosia::core::id(int n) { return ImHashData(&n, sizeof(n), seed()); }
void celosia::core::push_id(const char* label) { stack.back()->id_stack.push_back(id(label)); }
void celosia::core::push_id(int n) { stack.back()->id_stack.push_back(id(n)); }
void celosia::core::pop_id() { stack.back()->id_stack.pop_back(); }

ImFont* celosia::core::font() { return default_font ? default_font : resources::fonts::get("default"); }
float celosia::core::font_size() { return ImMax(1.f, font()->FontSize * font()->Scale); }
float celosia::core::frame_height() { return font_size() + style::frame::size_padding.y * 2.f; }

t_surface* celosia::core::surface() { return stack.empty() ? nullptr : stack.back(); }

t_surface* celosia::core::find(ImGuiID id) {
	const auto it = surfaces.find(id);
	return it != surfaces.end() ? it->second.get() : nullptr;
}

ImDrawList* celosia::core::drawlist() { return &stack.back()->drawlist; }
ImVec2 celosia::core::cursor() { return stack.back()->cursor; }

void celosia::core::set_cursor(ImVec2 local) {
	t_surface* s = stack.back();
	s->cursor = s->pos - ImVec2(0.f, s->scroll) + local;
}

void celosia::core::same_line(float spacing) {
	t_surface* s = stack.back();
	s->cursor = ImVec2(s->cursor_line_end.x + (spacing < 0.f ? style::general::spacing.x : spacing), s->cursor_line_end.y);
	s->line_height = s->line_height_last;
	s->line_text_offset = s->line_text_offset_last;
	s->same_line = true;
}

static ImVec2 region_max(const t_surface* s) { // the bottom right of where content goes, scrolled along with it
	return ImVec2(s->pos.x + s->size.x - s->padding.x - (s->scrollbar ? celosia::style::scrollbar::width : 0.f), s->pos.y - s->scroll + s->size.y - s->padding.y);
}

ImVec2 celosia::core::available() { return region_max(stack.back()) - stack.back()->cursor; }
float celosia::core::item_width() { return IM_TRUNC(stack.back()->item_width); }
float celosia::core::line_end() { return stack.back()->work.Max.x; }
float celosia::core::text_offset() { return stack.back()->line_text_offset; }

ImVec2 celosia::core::item_size(ImVec2 size, float default_w, float default_h) {
	const t_surface* s = stack.back();
	const ImVec2 max = region_max(s);
	size.x = size.x == 0.f ? default_w : size.x < 0.f ? ImMax(4.f, max.x - s->cursor.x + size.x) : size.x;
	size.y = size.y == 0.f ? default_h : size.y < 0.f ? ImMax(4.f, max.y - s->cursor.y + size.y) : size.y;
	return size;
}

void celosia::core::layout(const ImVec2& size, float text_baseline) {
	t_surface* s = stack.back();
	const float spacing = style::general::spacing.y;
	// an item whose text sits higher than the line's so far grows down by the difference, the text lines up
	const float baseline_offset = text_baseline >= 0.f ? ImMax(0.f, s->line_text_offset - text_baseline) : 0.f;
	const float line_top = s->same_line ? s->cursor_line_end.y : s->cursor.y;
	const float line_height = ImMax(s->line_height, s->cursor.y - line_top + size.y + baseline_offset);

	s->cursor_line_end = ImVec2(s->cursor.x + size.x, line_top);
	s->cursor = ImVec2(IM_TRUNC(s->pos.x + s->padding.x), IM_TRUNC(line_top + line_height + spacing)); // the next line, on whole pixels
	s->cursor_max = ImVec2(ImMax(s->cursor_max.x, s->cursor_line_end.x), ImMax(s->cursor_max.y, s->cursor.y - spacing));
	s->line_height_last = line_height;
	s->line_height = 0.f;
	s->line_text_offset_last = ImMax(s->line_text_offset, text_baseline);
	s->line_text_offset = 0.f;
	s->same_line = false;
}

void celosia::core::layout(const ImRect& bb, float text_baseline) { layout(bb.GetSize(), text_baseline); }

bool celosia::core::add(const ImRect& bb, ImGuiID id, int flags) {
	if (id != 0 && id == active_id)
		active_alive = true;
	if (id != 0 && !(flags & item_no_focus)) // clipped ones too, the keyboard scrolls to them
		focusables.push_back({ id, bb, stack.back(), popup_depth });
	// clipped away it's left out, unless it's held: a slider dragged out of view keeps following the mouse
	return bb.Overlaps(stack.back()->clip) || (id != 0 && id == active_id);
}

bool celosia::core::hoverable(const ImRect& bb, ImGuiID id) {
	t_surface* s = stack.back();
	if (s != hovered)
		return false;
	ImRect visible = bb;
	visible.ClipWith(s->clip);
	if (!visible.Contains(input::mouse_pos()))
		return false;
	if ((hovered_id != 0 && hovered_id != id) || (active_id != 0 && active_id != id))
		return false;
	if (id != 0)
		hovered_id = id;
	return true;
}

bool celosia::core::button(const ImRect& bb, ImGuiID id, bool* out_hovered, bool* out_held, int flags) {
	const bool is_hovered = hoverable(bb, id);
	if (is_hovered && input::mouse_clicked(input::left)) {
		active_id = id;
		active_alive = true;
		active_frame = frame_count;
		if (!(flags & item_no_focus)) // the keyboard carries on from here
			focus_id = id;
	}
	bool is_held = false, pressed = false;
	if (active_id == id) {
		if (input::mouse_down(input::left))
			is_held = true;
		else { // let go, a press when it's over the item
			pressed = is_hovered;
			active_id = 0;
		}
	}
	if (id == activate_id)
		pressed = true;
	if (pressed) {
		pressed_id = id;
		pressed_by_key = id == activate_id;
	}
	if (out_hovered) *out_hovered = is_hovered;
	if (out_held) *out_held = is_held;
	return pressed;
}

bool celosia::core::focused(ImGuiID id) { return keyboard_nav && focus_visible && focus_id == id; }

bool celosia::core::focus_key(ImGuiID id, int key) {
	if (!focused(id) || !input::focused() || !input::key_pressed(key))
		return false;
	if (key >= keys::left && key <= keys::down)
		arrows_taken[key - keys::left] = true;
	return true;
}

void celosia::core::default_focus(ImGuiID id) {
	focus_default = id;
	focus_default_level = popup_depth;
}

ImGuiID celosia::core::active() { return active_id == void_id ? 0 : active_id; }
bool celosia::core::activated(ImGuiID id) { return active_id == id && active_frame == frame_count; }
ImGuiStorage* celosia::core::storage() { return &storage_values; }
void celosia::core::hover(ImGuiID id) { hovered_id = id; }

t_surface* celosia::core::begin_surface(ImGuiID id, t_surface::e_kind kind, ImVec2 pos, ImVec2 size, ImVec2 padding) {
	std::unique_ptr<t_surface>& slot = surfaces[id];
	if (!slot)
		slot = std::make_unique<t_surface>();
	t_surface* s = slot.get();
	const bool created = s->frame == -1;
	const bool appearing = s->frame < frame_count - 1; // wasn't there last frame

	s->kind = kind;
	s->id = id;
	s->parent = kind == t_surface::scroll_area ? surface() : nullptr;
	(s->parent ? s->parent->children : roots).push_back(s);
	s->children.clear();
	s->id_stack.assign(1, id);
	s->frame = frame_count;
	s->last_used = ui::time;
	s->shown = !(kind == t_surface::popup && appearing) && (!s->parent || s->parent->shown);
	s->pos = ImTrunc(pos);
	s->size = size;
	s->padding = padding;
	s->item_width = kind == t_surface::popup ? IM_TRUNC(font_size() * 16.f) : IM_TRUNC(size.x * 0.65f);

	// a scrollbar when last frame's content didn't fit
	s->scrollbar = kind != t_surface::window && !created && s->content.y + padding.y * 2.f > size.y;
	s->inner = ImRect(s->pos, s->pos + size - ImVec2(s->scrollbar ? style::scrollbar::width : 0.f, 0.f));
	s->scroll_max = ImMax(0.f, s->content.y + padding.y * 2.f - s->inner.GetHeight());
	s->scroll = ImMin(s->scroll, s->scroll_max);

	// what's around it clips it: the viewport, a scroll area also what it's in. the hover rect is the part that shows
	const ImRect host = s->parent ? s->parent->clip : ImRect(ImVec2(0.f, 0.f), ui::display_size);
	s->hover_rect = ImRect(s->pos, s->pos + size);
	s->hover_rect.ClipWith(host);

	ImDrawList& dl = s->drawlist;
	dl._ResetForNewFrame();
	dl.PushTextureID(font()->ContainerAtlas->TexID);
	dl.PushClipRect(host.Min, host.Max, false);
	if (kind == t_surface::window)
		dl.AddRectFilled(s->pos, s->pos + size, style::window::background_color(), (float)style::general::rounding_window);

	stack.push_back(s);
	s->clip = host; // the scrollbar is outside of where the content goes
	if (wheel_target == id)
		scroll_by(s, input::wheel());
	if (kind != t_surface::window) {
		if (s->scrollbar)
			scrollbar(s);
		// eased towards where the wheel or the scrollbar sent it, on whole pixels
		s->scroll_target = ImClamp(s->scroll_target, 0.f, s->scroll_max);
		s->scroll_eased = s->scrollbar ? animations::functions::calc_smooth(ImMin(s->scroll_eased, s->scroll_max), s->scroll_target, style::general::scroll_speed) : s->scroll_target;
		s->scroll = ImMin(IM_ROUND(ImMax(s->scroll_eased, 0.f)), s->scroll_max);
	}

	// items are clipped half the padding in from the sides
	const float inset = IM_TRUNC(padding.x * 0.5f);
	s->clip = ImRect(ImTrunc(ImVec2(0.5f + s->inner.Min.x + inset, 0.5f + s->inner.Min.y)), ImTrunc(ImVec2(0.5f + s->inner.Max.x - inset, 0.5f + s->inner.Max.y)));
	s->clip.ClipWithFull(host);
	dl.PushClipRect(s->clip.Min, s->clip.Max, true);

	s->work.Min = ImTrunc(s->inner.Min - ImVec2(0.f, s->scroll) + padding);
	s->work.Max = s->work.Min + ImVec2(ImMax(0.f, size.x - padding.x * 2.f - (s->scrollbar ? style::scrollbar::width : 0.f)), ImMax(s->content.y, size.y - padding.y * 2.f));
	s->cursor_start = s->pos + padding - ImVec2(0.f, s->scroll);
	s->cursor = s->cursor_line_end = s->cursor_max = s->cursor_start;
	s->line_height = s->line_height_last = s->line_text_offset = s->line_text_offset_last = 0.f;
	s->same_line = false;
	return s;
}

void celosia::core::end_surface() {
	t_surface* s = stack.back();
	s->content = ImVec2(IM_TRUNC(s->cursor_max.x - s->cursor_start.x), IM_TRUNC(s->cursor_max.y - s->cursor_start.y));
	s->drawlist.PopClipRect();
	stack.pop_back();
}

void celosia::core::open_popup(ImGuiID id) {
	if ((int)popups.size() > popup_depth && popups[popup_depth].id == id)
		return; // already open
	popups.resize(popup_depth); // a popup opened at a level replaces the ones that were open there and over it
	popups.push_back({ id, frame_count, -1, pressed_id ? pressed_id : focus_id, pressed_id && pressed_by_key });
}

bool celosia::core::popup_open(ImGuiID id) { return (int)popups.size() > popup_depth && popups[popup_depth].id == id; }
void celosia::core::close_popups(int level) {
	level = ImMax(level, 0);
	if (level >= (int)popups.size())
		return;
	if (focus_level > level) { // the keyboard was in what closes, it goes back to what opened it
		focus_id = popups[level].opener;
		focus_level = level;
	}
	popups.resize(level);
}
void celosia::core::close_popup() { close_popups(popup_depth - 1); }
int celosia::core::popup_level() { return popup_depth; }

void celosia::core::begin_popup() {
	popups[popup_depth].begin_frame = frame_count;
	popup_depth++;
}

void celosia::core::end_popup() { popup_depth--; }

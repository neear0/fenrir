#pragma once
#define IMGUI_DEFINE_MATH_OPERATORS
#define _CRT_SECURE_NO_WARNINGS

#include <iostream>
#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <map>
#include <vector>

#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_internal.h"
#include "../external/imgui/misc/freetype/imgui_freetype.h"

#include "style/style.h"
#include "core/core.h"
#include "render/render.h"
#include "debug/debug.h"
#include "widgets/widgets.h"

namespace celosia {
	namespace initialize {
		void context();
		void fonts(); // after platform::create(), the sizes follow style::scale
		void images(); // after platform::create(), the textures need the renderer
	}

	namespace resources {
		namespace fonts {
			struct t_font {
				const unsigned char* data; // compiled in, see font_data
				int data_size;
				float size;                // in pixels at scale 1, add() scales it by style::scale
				unsigned int hinting = ImGuiFreeTypeBuilderFlags_LightHinting; // how freetype fits glyphs to the pixel grid, ImGuiFreeTypeBuilderFlags
			};

			ImFont* add(const std::string& name, const t_font& font); // null when the font couldn't be loaded
			ImFont* get(const std::string& name); // the first one added when there's none by that name
			inline std::unordered_map<std::string, ImFont*> map;
		}

		namespace images {
			struct t_image {
				ImTextureID texture = nullptr;
				ImVec2 size;   // in pixels
			};

			bool add(const std::string& name, const unsigned char* png, int png_size); // decodes a compiled in png and uploads it, after platform::create()
			const t_image& get(const std::string& name); // one with a null texture when there's none by that name
			void clear();  // frees every texture, platform::destroy() calls it
			inline std::unordered_map<std::string, t_image> map;
		}

		namespace font_data { // compiled into the binary, see resources/embed_resources.cmake
			extern const unsigned char poppins_regular[];
			extern const int poppins_regular_size;
			extern const unsigned char poppins_bold[];
			extern const int poppins_bold_size;
		}

		namespace image_data { // compiled into the binary, see resources/embed_resources.cmake
			extern const unsigned char test[];
			extern const int test_size;
		}
	}

	namespace variables {
		struct t_title {
			const char* name;   // in the text color
			const char* accent; // right after it, in the accent
		};
		inline t_title title = { "celos", "UI" }; // shown in the titlebar
		
		namespace config {
			inline std::unordered_map<std::string, int> ints;
			inline std::unordered_map<std::string, float> floats;
			inline std::unordered_map<std::string, bool> bools;
			inline std::unordered_map<std::string, ImVec2> vec2;
			inline std::unordered_map<std::string, std::string> strings;
		}

		namespace temporary {
			inline std::unordered_map<std::string, int> ints;
			inline std::unordered_map<std::string, float> floats;
			inline std::unordered_map<std::string, bool> bools;
			inline std::unordered_map<std::string, ImVec2> vec2;
			inline std::unordered_map<std::string, std::string> strings;
		}
	}

	namespace animations {
		struct t_value {
			ImVec4 value;            // floats use x and ImVec2 uses x, y
			double last_used = 0.0;  // ui::time when it was last set
		};
		inline std::unordered_map<ImGuiID, t_value> values; // one entry per animated value
		inline double forget_after = 10.0; // seconds a value can go unused before it's dropped, it starts at its goal again when it's back
		inline float base_speed = 2.f;
		inline float delta_time = 0.f; // the frame time animations step by, ui::delta_time evened out, see new_frame()
		inline float color_speed = 10.f; // hover and press colors ease by this, most of the way in about a quarter second

		enum e_method{ smooth, linear };

		void new_frame(); // updates delta_time, ui::begin() calls it every frame

		namespace functions {
			float calc_linear(float current_value, const float goal_value, float speed);
			float calc_smooth(float current_value, const float goal_value, float speed);

			void tab_switch();
		}

		// items key their animations by their id (core::id()), so items with the same label in different surfaces don't share one:
		// key("button_glow", id). things that exist once can use a name: key("groupbox_active_spacing")
		ImGuiID key(std::string_view name, ImGuiID seed = 0);
		float get(ImGuiID id);              // 0 when there's nothing by that key
		void snap(ImGuiID id, float value); // sets it at once, without easing

		// eases the value towards goal_value a step and returns it, a new value starts at goal_value
		float set(ImGuiID id, const float goal_value, float speed = 0, e_method method = e_method::smooth);
		ImVec2 set(ImGuiID id, const ImVec2 goal_value, float speed = 0, e_method method = e_method::smooth);
		ImVec4 set(ImGuiID id, const ImVec4 goal_value, float speed = 0, e_method method = e_method::smooth);
	}

	namespace ui {
		inline ImVec2 size = { 800, 600 };
		inline bool active = true;

		inline double time = 0.0;          // seconds since the first frame, begin() moves it on
		inline float delta_time = 0.f;     // seconds the last frame took
		inline ImVec2 display_size;        // the window's drawable area, 0 while it's minimized. begin() asks the platform
		inline ImVec2 framebuffer_scale = { 1.f, 1.f }; // framebuffer pixels per unit of ui::size

		inline std::string tab_active;  // last tab button pressed
		inline std::string tab_current; // tab being drawn, follows tab_active once the switch animation hides the old one

		void begin();
		void end();
		void render();
	}

	namespace functions {
		ImVec2 mouse_vec2();
	}

	namespace inputsystem {
		namespace registry { // indexed by virtual key code
			inline std::array<bool, 256> watched{};  // keys refresh() keeps track of
			inline std::array<bool, 256> state{};    // held state of watched keys this frame
			inline std::array<bool, 256> previous{}; // and last frame
		}

		namespace key {
			bool down(int key); // went down since last frame, the same answer for every caller during a frame
			bool up(int key);   // went up since last frame
			bool held(int key); // down right now, asks the platform every time

			void watch(int key);   // down() and up() watch keys on their first call, which reports nothing for that frame. watch a key beforehand to avoid that
			void unwatch(int key);
		}

		void refresh();		// this has to be ran every frame
	}

	// the window's own mouse and keyboard, from the platform's events for the window: the mouse while it's over the window
	// or a button is held on it, the keys while the window has the focus. inputsystem asks about keys anywhere, at any time
	namespace input {
		enum e_button { left, right, middle, button_count };

		// the platform's side, as the events come in
		void add_mouse_pos(ImVec2 pos);   // in the window, -FLT_MAX once the mouse left it
		void add_mouse_button(int button, bool down);
		void add_wheel(float notches);    // positive is away from the user, scrolling up
		void add_key(int key, bool down); // celosia::keys code, a key held down comes again as it repeats
		void add_focus(bool focused);     // losing it lets go of the keys and buttons, without them counting as released

		// applies the events since the last frame, ui::begin() calls it. a button that went down and up between two frames
		// is down for this one and up for the next, a quick click isn't lost
		void new_frame();

		// the ui's side, the same answers for the whole frame
		ImVec2 mouse_pos();
		bool mouse_down(int button);
		bool mouse_clicked(int button);  // went down this frame
		bool mouse_released(int button); // went up this frame
		float wheel();                   // notches this frame
		bool key_down(int key);
		bool key_pressed(int key, bool repeat = true); // went down this frame, or repeated unless repeat is false
		bool focused();
	}

	namespace keys { // key codes used by inputsystem, these match the windows virtual key codes and other platforms translate them
		enum : int {
			mouse_left = 0x01, mouse_right = 0x02, mouse_middle = 0x04,
			backspace = 0x08, tab = 0x09, enter = 0x0D, shift = 0x10, control = 0x11, alt = 0x12, escape = 0x1B, space = 0x20,
			left = 0x25, up = 0x26, right = 0x27, down = 0x28, insert = 0x2D, del = 0x2E, home = 0x24, end = 0x23,
			// '0'-'9' and 'A'-'Z' are their ascii values
			f1 = 0x70, // f1 + n for f(n + 1), up to f12
		};
	}

	namespace effects { // shader effects (shaders/*.hlsl). each one is a draw list callback that swaps in its shader, so it lands in order between the draw list's own draws
		enum class e_type { gradient, glow, blur, background, noise, count };

		struct t_constants { // matches effect_constants in shaders/effect.hlsli
			ImVec4 reserved; // vulkan: ImGui's vertex shader keeps its push constants here
			ImVec4 rect;     // min x, min y, max x, max y in framebuffer pixels
			ImVec4 color_a;
			ImVec4 color_b;
			ImVec4 color_c;
			ImVec4 params;   // x is the corner rounding in pixels, the rest depends on the effect
		};

		struct t_command {
			e_type type;
			t_constants constants;
		};
		inline std::vector<t_command> commands; // this frame's effects, the callbacks refer to them by index

		void new_frame(); // forgets last frame's effects, called by ui::begin()

		// positions, rounding and radius are in units of ui::size like the rest of the draw list
		void gradient(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor a, ImColor b, float rounding = 0.f, float angle = 0.f, float speed = 0.f); // a to b along angle (radians), speed > 0 slides it back and forth
		void glow(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor color, float rounding = 0.f, float radius = 12.f); // around min-max, nothing inside it, draw it before the item
		void blur(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor tint, float rounding = 0.f, float radius = 10.f); // frosted glass over what's already drawn there, tint alpha is how much tint
		void background(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor base, ImColor one, ImColor two, float rounding = 0.f); // blobs of one and two drifting over base, their alpha is their strength
		void noise(ImDrawList* drawlist, ImVec2 min, ImVec2 max, float strength = 0.05f, float rounding = 0.f, float size = 1.f, bool animated = false); // film grain over what's already drawn, size is how big a grain is, animated changes the pattern 24 times a second
	}

	namespace platform { // one implementation per backend: win32/ (Win32 + D3D11) and glfw/ (GLFW + Vulkan)
		bool create(const char* title, ImVec2 pos, ImVec2 size); // window, device and ImGui's renderer, call after initialize::context(). size is at scale 1, it becomes ui::size times style::scale (the window and the layout), see style::set_scale()
		bool poll();         // handles window events, false once the window has been closed
		void new_frame();
		void present();      // renders core::draw_data() and shows it
		ImVec2 display_size();      // the window's drawable area in units of ui::size, 0 while it's minimized
		ImVec2 framebuffer_scale(); // framebuffer pixels per unit
		void destroy();

		bool key_held(int key);  // celosia::keys code, works while the window isn't focused where the platform allows it
		ImVec2 cursor_pos();     // screen coordinates
		ImRect window_rect();    // screen coordinates
		ImVec2 screen_size();
		void move(ImVec2 pos);
		void bring_to_top();

		bool blur_supported(); // the os can blur what's behind the window (windows, kwin on x11), known once create() ran, see style::window::background
		void set_blur(bool enabled); // on x11 the blurred area follows style::general::rounding_window, call it again after changing that

		// textures for images, after create(). rgba is 4 bytes per pixel, row after row
		ImTextureID create_texture(const unsigned char* rgba, int width, int height); // null when it couldn't be made
		void destroy_texture(ImTextureID texture);

		// renderer side of effects
		void draw_effect(const ImDrawList* drawlist, const ImDrawCmd* command); // ImDrawCallback, switches to the shader of effects::commands[command->UserCallbackData]
		ImTextureID backdrop_texture(); // copy of the frame drawn so far, refreshed by every blur. null when the renderer can't copy the frame
	}

	namespace window {
		void drag(); // moves the window while the titlebar is held
	}
}
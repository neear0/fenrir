#pragma once

#include "../ui.h"
#include <Windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>

#include "../../external/imgui/dx11/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

namespace celosia_win32 {
	namespace d3d {
		inline ID3D11Device* device;
		inline ID3D11DeviceContext* device_context;
		inline IDXGISwapChain* swapchain;
		inline ID3D11RenderTargetView* render_target_view;

		bool create_device(HWND hwnd);
		void destroy_device();
		void report_debug_messages(); // debug builds: prints what the d3d11 debug layer reported since the last call
	}

	namespace effect_renderer { // d3d11 side of celosia::effects, see effects_d3d11.cpp
		bool create();
		void update_backdrop(); // before each frame, keeps the backdrop the size of the back buffer
		void destroy();
	}

	namespace variables {
		inline std::map<std::wstring, std::pair<WNDCLASSEXW, HWND>> windows; // by title, which is also the class name
		inline long default_window_flags = WS_EX_TOPMOST | WS_EX_LAYERED; // WS_EX_TRANSPARENT to be able to click through
	}

	namespace window {
		LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);
		HWND create(LPCWSTR window_title, ImVec2 pos, ImVec2 size, long window_flags = variables::default_window_flags);
		void enable_transparency(HWND hwnd);
		void enable_blur(HWND hwnd, int accent = 4); // 4 is the acrylic blur, 0 turns it off
		void show(HWND hwnd);
		void move(HWND hwnd, ImVec2 pos);
		void destroy();
	}
}
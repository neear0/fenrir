#include "../win32/ui_win32.h"

static bool mouse_tracked = false; // TrackMouseEvent() asked for a WM_MOUSELEAVE
static int buttons_down = 0;       // a bit per button

static void mouse_button(HWND hwnd, int button, bool down) { // the window keeps getting the mouse while a button is held on it, also outside of it
    if (down && buttons_down == 0 && GetCapture() == nullptr)
        SetCapture(hwnd);
    buttons_down = down ? buttons_down | (1 << button) : buttons_down & ~(1 << button);
    if (!down && buttons_down == 0 && GetCapture() == hwnd)
        ReleaseCapture();
    celosia::input::add_mouse_button(button, down);
}

LRESULT WINAPI celosia_win32::window::wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!mouse_tracked) {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
            mouse_tracked = TrackMouseEvent(&track) != FALSE;
        }
        celosia::input::add_mouse_pos(ImVec2((float)GET_X_LPARAM(l_param), (float)GET_Y_LPARAM(l_param)));
        return 0;
    case WM_MOUSELEAVE:
        mouse_tracked = false;
        celosia::input::add_mouse_pos(ImVec2(-FLT_MAX, -FLT_MAX));
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: mouse_button(hwnd, celosia::input::left, true); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: mouse_button(hwnd, celosia::input::right, true); return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: mouse_button(hwnd, celosia::input::middle, true); return 0;
    case WM_LBUTTONUP: mouse_button(hwnd, celosia::input::left, false); return 0;
    case WM_RBUTTONUP: mouse_button(hwnd, celosia::input::right, false); return 0;
    case WM_MBUTTONUP: mouse_button(hwnd, celosia::input::middle, false); return 0;
    case WM_MOUSEWHEEL:
        celosia::input::add_wheel((float)GET_WHEEL_DELTA_WPARAM(w_param) / (float)WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (w_param < 256) // celosia::keys are the virtual key codes
            celosia::input::add_key((int)w_param, msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        break; // alt+f4 and the like still work
    case WM_SETFOCUS: celosia::input::add_focus(true); return 0;
    case WM_KILLFOCUS:
        buttons_down = 0; // input lets go of them
        if (GetCapture() == hwnd)
            ReleaseCapture();
        celosia::input::add_focus(false);
        return 0;
    case WM_SYSCOMMAND:
        if ((w_param & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, w_param, l_param);
}

HWND celosia_win32::window::create(LPCWSTR window_title, ImVec2 pos, ImVec2 size, long window_flags) {
    if (const auto existing = variables::windows.find(window_title); existing != variables::windows.end()) { // one with the same title / class is replaced
        DestroyWindow(existing->second.second);
        UnregisterClassW(window_title, existing->second.first.hInstance);
        variables::windows.erase(existing);
    }

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_HREDRAW | CS_VREDRAW,
        wnd_proc,
        0L, 0L,
        GetModuleHandle(nullptr),
        nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr,
        window_title,
        nullptr
    };

    if (!RegisterClassExW(&wc))
        return nullptr;

    const HWND hwnd = CreateWindowExW(
        window_flags,
        wc.lpszClassName,
        window_title,
        WS_POPUP,
        (int)pos.x, (int)pos.y, (int)size.x, (int)size.y,
        nullptr, nullptr,
        wc.hInstance,
        nullptr
    );
    if (!hwnd) {
        UnregisterClassW(window_title, wc.hInstance);
        return nullptr;
    }

    variables::windows[window_title] = { wc, hwnd };
    return hwnd;
}

void celosia_win32::window::show(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
}

void celosia_win32::window::move(HWND hwnd, ImVec2 pos) {
    SetWindowPos(hwnd, nullptr, (int)pos.x, (int)pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); // only moves it
}

void celosia_win32::window::destroy() { // every window made through create(), along with its class
    for (auto const& [title, window] : variables::windows) {
        DestroyWindow(window.second); // fails harmlessly if the window was already closed
        UnregisterClassW(title.c_str(), window.first.hInstance);
    }
    variables::windows.clear();
}

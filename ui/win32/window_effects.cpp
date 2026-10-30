#include "../win32/ui_win32.h"

void celosia_win32::window::enable_transparency(HWND hwnd) { // the frame's alpha is what the desktop shows of it
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);
    const MARGINS margin{ -1, -1, -1, -1 }; // the whole window composes with alpha
    DwmExtendFrameIntoClientArea(hwnd, &margin);
}

void celosia_win32::window::enable_blur(HWND hwnd, int accent) {
    struct accent_policy {
        int accent_state;
        int accent_flags;
        int gradient_color;
        int animation_id;
    };
    struct win_comp_attr_data {
        int attribute;
        PVOID data;
        ULONG size;
    };

    const HINSTANCE hm = LoadLibrary(L"user32.dll");
    if (hm) {
        typedef BOOL(WINAPI* set_window_composition_attribute_fn)(HWND, win_comp_attr_data*);

        const set_window_composition_attribute_fn set_window_composition_attribute = (set_window_composition_attribute_fn)GetProcAddress(hm, "SetWindowCompositionAttribute");
        if (set_window_composition_attribute) {
            // the acrylic blur (4) doesn't blur at all with a fully transparent tint, an alpha of 1 is invisible but turns it on
            accent_policy policy = { accent, 0, 0x01000000, 0 };
            win_comp_attr_data data = { 19, &policy,sizeof(accent_policy) };
            set_window_composition_attribute(hwnd, &data);
        }
        FreeLibrary(hm);
    }
}


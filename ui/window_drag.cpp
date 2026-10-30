#include "ui.h"

static bool dragging = false;
static ImVec2 offset;

void celosia::window::drag() { // ui::render() calls it while the ui is visible
    const ImRect window_rect = platform::window_rect();
    const ImVec2 mouse_pos = functions::mouse_vec2();

    if (dragging) {
        dragging = inputsystem::key::held(keys::mouse_left);
        if (dragging) // once released the window stays where it is, even if the cursor moved since the last frame
            platform::move(ImVec2(mouse_pos.x - offset.x, mouse_pos.y - offset.y));
        return;
    }

    if ((mouse_pos.x >= window_rect.Min.x && mouse_pos.x <= window_rect.Max.x) &&
        (mouse_pos.y >= window_rect.Min.y && mouse_pos.y <= window_rect.Min.y + style::titlebar::height)) {
        if (inputsystem::key::down(keys::mouse_left)) {
            dragging = true;
            offset = { mouse_pos.x - window_rect.Min.x, mouse_pos.y - window_rect.Min.y };
        }
    }
}

#include "ui.h"

// the window's input: the platform adds events as they arrive and new_frame() applies them in order. a mouse button
// changing a second time, or the mouse moving after a button changed, waits for the next frame, so a click that went down
// and up between two frames is still seen, where it happened

struct t_event {
    enum e_kind { mouse_pos, mouse_button, wheel, key, focus } kind;
    ImVec2 pos;   // mouse_pos
    int code;     // mouse_button: the button, key: the key
    bool down;    // mouse_button, key, focus: focused
    float amount; // wheel
};
static std::vector<t_event> events;

static ImVec2 mouse(-FLT_MAX, -FLT_MAX);
static std::array<bool, celosia::input::button_count> buttons_down{}, buttons_clicked{}, buttons_released{};
static float wheel_notches = 0.f;
static std::array<bool, 256> keys_down{}, keys_pressed{}, keys_pressed_first{}; // first: not a repeat
static bool has_focus = false;

void celosia::input::add_mouse_pos(ImVec2 pos) { events.push_back({ t_event::mouse_pos, pos, 0, false, 0.f }); }

void celosia::input::add_mouse_button(int button, bool down) {
    if (button >= 0 && button < button_count)
        events.push_back({ t_event::mouse_button, ImVec2(), button, down, 0.f });
}

void celosia::input::add_wheel(float notches) { events.push_back({ t_event::wheel, ImVec2(), 0, false, notches }); }
void celosia::input::add_key(int key, bool down) { events.push_back({ t_event::key, ImVec2(), key & 0xFF, down, 0.f }); }
void celosia::input::add_focus(bool focused) { events.push_back({ t_event::focus, ImVec2(), 0, focused, 0.f }); }

void celosia::input::new_frame() {
    buttons_clicked.fill(false);
    buttons_released.fill(false);
    keys_pressed.fill(false);
    keys_pressed_first.fill(false);
    wheel_notches = 0.f;

    bool button_changed = false, moved = false, wheeled = false;
    std::array<bool, button_count> changed{};
    size_t applied = 0;
    for (; applied < events.size(); applied++) {
        const t_event& e = events[applied];
        if (e.kind == t_event::mouse_pos) {
            if (button_changed || wheeled) // the click lands where the mouse was when it happened
                break;
            mouse = e.pos;
            moved = true;
        }
        else if (e.kind == t_event::mouse_button) {
            if (changed[e.code] || wheeled)
                break;
            if (buttons_down[e.code] != e.down) {
                buttons_down[e.code] = e.down;
                (e.down ? buttons_clicked : buttons_released)[e.code] = true;
                changed[e.code] = button_changed = true;
            }
        }
        else if (e.kind == t_event::wheel) {
            if (moved || button_changed)
                break;
            wheel_notches += e.amount;
            wheeled = true;
        }
        else if (e.kind == t_event::key) {
            if (e.down) {
                keys_pressed[e.code] = true; // a repeat as well
                keys_pressed_first[e.code] |= !keys_down[e.code];
            }
            keys_down[e.code] = e.down;
        }
        else if (e.kind == t_event::focus) {
            has_focus = e.down;
            if (!has_focus) { // what goes up while another window has the focus never arrives. let go of, not released: no click
                keys_down.fill(false);
                buttons_down.fill(false);
                mouse = ImVec2(-FLT_MAX, -FLT_MAX);
            }
        }
    }
    events.erase(events.begin(), events.begin() + applied);
}

ImVec2 celosia::input::mouse_pos() { return mouse; }
bool celosia::input::mouse_down(int button) { return button >= 0 && button < button_count && buttons_down[button]; }
bool celosia::input::mouse_clicked(int button) { return button >= 0 && button < button_count && buttons_clicked[button]; }
bool celosia::input::mouse_released(int button) { return button >= 0 && button < button_count && buttons_released[button]; }
float celosia::input::wheel() { return wheel_notches; }
bool celosia::input::key_down(int key) { return keys_down[key & 0xFF]; }
bool celosia::input::key_pressed(int key, bool repeat) { return (repeat ? keys_pressed : keys_pressed_first)[key & 0xFF]; }
bool celosia::input::focused() { return has_focus; }

#include "ui.h"

void celosia::initialize::context() {
    ImGui::CreateContext(); // for its font atlas, and the renderers keep their state in it

    style::themes::initialize();
    style::themes::set(style::themes::dark, true);

    inputsystem::key::watch(keys::mouse_left);
    // platform::create() switches style::window::background to the shader when nothing can blur the desktop
}

void celosia::initialize::fonts() {
    using namespace resources::font_data;
    resources::fonts::add("default", { poppins_regular, poppins_regular_size, 16.f }); // the first one is ImGui's default font
    resources::fonts::add("default_smaller", { poppins_regular, poppins_regular_size, 14.f });
    resources::fonts::add("title", { poppins_bold, poppins_bold_size, 24.f });
}

void celosia::initialize::images() {
    using namespace resources::image_data;
    resources::images::add("test", test, test_size);
}

void celosia::ui::begin() {
    static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    delta_time = now > time ? (float)(now - time) : 1.f / 60.f; // the first frame has nothing to measure against
    time = now;
    display_size = platform::display_size();
    framebuffer_scale = platform::framebuffer_scale();

    effects::new_frame();
    inputsystem::refresh();
    input::new_frame();
    style::themes::update();

    platform::new_frame(); // builds the font atlas the first time, core needs it
    core::new_frame();
    animations::new_frame();
}

void celosia::ui::end() {
    core::end_frame();
    platform::present();
}

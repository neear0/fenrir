#include "ui.h"

void celosia::ui::render() {
    begin();

    render::tab_content.drawlist = nullptr;
    render::tab_content.children.resize(0);

    if (ui::active) {
        render::groupbox::index = 0;
        render::main_window();
        window::drag();
    }

    animations::functions::tab_switch();
    end();
}

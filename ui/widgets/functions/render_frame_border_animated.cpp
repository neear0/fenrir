#include "../../ui.h"

void celosia::widgets::render_frame_border_animated(ImGuiID id, ImVec2 p_min, ImVec2 p_max, ImColor fill_col, float rounding) // any component using this will be animated with no need for any further setup
{

    ImColor animated_color = celosia::animations::set(celosia::animations::key("render_frame_border_animated", id), ImVec4(fill_col), celosia::animations::color_speed);

    const float border_size = celosia::style::frame::border_size;
    core::drawlist()->AddRect(p_min, p_max, animated_color, rounding, 0, border_size);
}
#include "../../ui.h"

void celosia::widgets::render_frame_animated(ImGuiID id, ImVec2 p_min, ImVec2 p_max, ImColor fill_col, float rounding)
{

    ImColor animated_color = celosia::animations::set(celosia::animations::key("render_frame_animated", id), ImVec4(fill_col), celosia::animations::color_speed);
    core::drawlist()->AddRectFilled(p_min, p_max, animated_color, rounding);
}
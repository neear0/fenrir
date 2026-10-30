#include "../../ui.h"

void celosia::widgets::render_frame_border_color(ImVec2 p_min, ImVec2 p_max, ImColor clr,float rounding)
{
    const float border_size = celosia::style::frame::border_size;
    if (border_size > 0)
    {
        core::drawlist()->AddRect(p_min, p_max, clr, rounding, 0, border_size);
    }
}

#include "../ui.h"

void celosia::widgets::image(const char* name, ImVec2 size, float rounding) {
    const celosia::resources::images::t_image image = celosia::resources::images::get(name); // a copy, it's a pointer and a size
    if (!image.texture)
        return;
    if (size.x <= 0.f || size.y <= 0.f)
        size = image.size;

    const ImRect bb(core::cursor(), core::cursor() + size);
    core::layout(bb);
    if (!core::add(bb, 0))
        return;
    core::drawlist()->AddImageRounded(image.texture, bb.Min, bb.Max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding);
}

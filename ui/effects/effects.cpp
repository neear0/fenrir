#include "../ui.h"

// the backend independent half of the effects: fills in the shader constants and puts each effect into the draw list as
// callback (switch to the effect's shader) -> geometry covering the effect -> callback (back to ImGui's shader).
// the renderer half is platform::draw_effect() in win32/effects_d3d11.cpp and glfw/effects_vulkan.cpp

static ImVec2 to_framebuffer(ImVec2 p) { // the shaders work in framebuffer pixels
    return ImVec2(p.x * celosia::ui::framebuffer_scale.x, p.y * celosia::ui::framebuffer_scale.y);
}

static float to_pixels(float size) {
    return size * celosia::ui::framebuffer_scale.x;
}

static celosia::effects::t_constants constants(ImVec2 min, ImVec2 max, float rounding) {
    celosia::effects::t_constants c = {};
    const ImVec2 a = to_framebuffer(min), b = to_framebuffer(max);
    c.rect = ImVec4(a.x, a.y, b.x, b.y);
    c.params.x = to_pixels(rounding);
    return c;
}

static void add(ImDrawList* drawlist, celosia::effects::e_type type, const celosia::effects::t_constants& c, ImVec2 min, ImVec2 max, ImTextureID texture = nullptr) {
    celosia::effects::commands.push_back({ type, c });
    drawlist->AddCallback(celosia::platform::draw_effect, (void*)(intptr_t)(celosia::effects::commands.size() - 1));
    if (texture)
        drawlist->AddImage(texture, min, max); // the renderer binds the texture of the draw, so the blur gets its backdrop
    else
        drawlist->AddRectFilled(min, max, IM_COL32_WHITE);
    drawlist->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void celosia::effects::new_frame() {
    commands.clear();
}

void celosia::effects::gradient(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor a, ImColor b, float rounding, float angle, float speed) {
    t_constants c = constants(min, max, rounding);
    c.color_a = a;
    c.color_b = b;
    c.params.y = angle;
    c.params.z = speed;
    c.params.w = (float)ui::time;
    add(drawlist, e_type::gradient, c, min, max);
}

void celosia::effects::glow(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor color, float rounding, float radius) {
    t_constants c = constants(min, max, rounding);
    c.color_a = color;
    c.params.y = to_pixels(radius);

    const ImVec2 reach(radius, radius);
    drawlist->PushClipRect(drawlist->GetClipRectMin() - reach, drawlist->GetClipRectMax() + reach); // the glow may spill a little past the clip rect
    add(drawlist, e_type::glow, c, min - reach, max + reach);
    drawlist->PopClipRect();
}

void celosia::effects::blur(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor tint, float rounding, float radius) {
    const ImTextureID backdrop = platform::backdrop_texture();
    if (!backdrop) { // the renderer can't copy the frame, a plain tinted panel instead
        drawlist->AddRectFilled(min, max, tint, rounding);
        return;
    }

    t_constants c = constants(min, max, rounding);
    const ImVec2 framebuffer = to_framebuffer(ui::display_size);
    c.color_a = tint;
    c.params.y = to_pixels(radius);
    c.params.z = 1.f / ImMax(framebuffer.x, 1.f);
    c.params.w = 1.f / ImMax(framebuffer.y, 1.f);
    add(drawlist, e_type::blur, c, min, max, backdrop);
}

void celosia::effects::background(ImDrawList* drawlist, ImVec2 min, ImVec2 max, ImColor base, ImColor one, ImColor two, float rounding) {
    t_constants c = constants(min, max, rounding);
    c.color_a = base;
    c.color_b = one;
    c.color_c = two;
    c.params.w = (float)ui::time;
    add(drawlist, e_type::background, c, min, max);
}

void celosia::effects::noise(ImDrawList* drawlist, ImVec2 min, ImVec2 max, float strength, float rounding, float size, bool animated) {
    if (strength <= 0.f)
        return;
    t_constants c = constants(min, max, rounding);
    c.params.y = to_pixels(size);
    c.params.z = strength;
    c.params.w = animated ? ImFloor((float)ui::time * 24.f) : 0.f; // a new pattern 24 times a second, like film
    add(drawlist, e_type::noise, c, min, max);
}

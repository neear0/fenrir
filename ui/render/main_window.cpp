#include "../ui.h"

// the demo window: the background, titlebar and sidebar, and the open tab's groupboxes. Widgets shows what there is,
// Appearance is a settings page for the look: presets first, the picker and the sliders for going further, all of them
// only as far as style::limits lets them

static float color[4] = { celosia::style::general::main_color.Value.x, celosia::style::general::main_color.Value.y, celosia::style::general::main_color.Value.z, celosia::style::general::main_color.Value.w };

static void widgets_tab() {
    using namespace celosia;
    render::groupbox::begin("Buttons", "", render::groupbox::e_group_layout::four);
    for (int i = 0; i < 20; i++)
        if (widgets::button(("Button " + std::to_string(i)).c_str()))
            debug::log("Button " + std::to_string(i) + " has been hit");
    render::groupbox::end();

    render::groupbox::begin("Checkboxes", "", render::groupbox::e_group_layout::four);
    for (int i = 0; i < 20; i++)
        widgets::checkbox_map(("Checkbox " + std::to_string(i)).c_str());
    render::groupbox::end();

    render::groupbox::begin("Inputs", "", render::groupbox::e_group_layout::four);
    const char* items[] = { "Item A", "Item B", "Item C" };
    widgets::combo("Combo", &variables::config::ints["demo_combo"], items, IM_ARRAYSIZE(items), IM_ARRAYSIZE(items));
    widgets::slider_float("Slider", &variables::config::floats["demo_slider"], 0.f, 100.f, "%.0f");
    widgets::toggle("Toggle", &variables::config::bools["demo_toggle"]);
    render::groupbox::end();

    render::groupbox::begin("Image", "", render::groupbox::e_group_layout::four);
    const ImVec2 picture = resources::images::get("test").size; // as big as fits in the groupbox, keeping its aspect
    const ImVec2 room = core::available();
    if (picture.x > 0.f && picture.y > 0.f) {
        const float fit = ImMin(1.f, ImMin(room.x / picture.x, room.y / picture.y));
        if (fit > 0.f)
            widgets::image("test", picture * fit, 8.f * style::scale);
    }
    render::groupbox::end();
}

static void appearance_tab() {
    using namespace celosia;
    render::groupbox::begin("Style", "Theme, accent, titles, grain and the keyboard");
    const char* themes[] = { "Dark", "Light" };
    int theme = style::themes::active.name == style::themes::light.name ? 1 : 0;
    if (widgets::segmented("Theme", &theme, themes, IM_ARRAYSIZE(themes)))
        style::themes::set(theme == 1 ? style::themes::light : style::themes::dark);

    widgets::swatches("##accent", color, style::presets::accents, IM_ARRAYSIZE(style::presets::accents));
    core::same_line(style::general::inner_spacing.x);
    widgets::color_edit("Accent", color, false, style::limits::saturation_min, style::limits::value_min);
    style::general::main_color = { color[0], color[1], color[2], color[3] };

    const char* titles[] = { "Full", "Minimal" };
    int title = style::groupbox::title_style == style::groupbox::minimal ? 1 : 0;
    if (widgets::segmented("Titles", &title, titles, IM_ARRAYSIZE(titles)))
        style::groupbox::title_style = title == 1 ? style::groupbox::minimal : style::groupbox::full;

    bool grain = style::window::noise > 0.f;
    if (widgets::toggle("Film grain", &grain))
        style::window::noise = grain ? style::presets::noise : 0.f;
    core::same_line(style::frame::size_padding.x * 2.f);
    widgets::toggle("Keyboard navigation", &core::keyboard_nav); // tab, the arrows, enter, space and escape, see core.h
    render::groupbox::end();

    render::groupbox::begin("Glass", "What shows through the window");
    if (platform::blur_supported()) { // otherwise there's only the shader background
        const char* backgrounds[] = { "Blur", "Shader" };
        int background = style::window::background == style::window::shader ? 1 : 0;
        if (widgets::segmented("Background", &background, backgrounds, IM_ARRAYSIZE(backgrounds))) {
            style::window::background = background == 1 ? style::window::shader : style::window::blur;
            platform::set_blur(style::window::background == style::window::blur);
        }
    }

    const char* glass_names[IM_ARRAYSIZE(style::presets::glass)];
    for (int i = 0; i < IM_ARRAYSIZE(style::presets::glass); i++)
        glass_names[i] = style::presets::glass[i].name;
    int glass = style::presets::glass_current();
    if (widgets::segmented("Glass", &glass, glass_names, IM_ARRAYSIZE(glass_names)))
        style::presets::set_glass(style::presets::glass[glass]);

    widgets::slider_float("Glass blur", &style::window::glass_blur, style::limits::glass_blur_min * style::scale, style::limits::glass_blur_max * style::scale, "%.0f px");
    if (style::window::background == style::window::blur) // the shader background is opaque, there's nothing behind to see
        widgets::slider_float("Glass opacity", &style::window::glass_opacity, style::limits::glass_opacity_min, 1.f, "%.2f");
    render::groupbox::end();
}

void celosia::render::main_window() {
    const ImVec2 pos(0.f, 0.f);
    window::begin(variables::title.name, pos, ui::size);

    ImDrawList* drawlist = core::drawlist();
    if (style::window::background == style::window::shader) {
        const ImVec4 main = style::general::accent.Value;
        const ImVec4 accent = style::themes::rgb_accent(style::general::accent).Value;
        ImVec4 base = style::themes::active.background.Value;
        base.w = 1.f; // the window is opaque with the shader background, only the rounded corners let the desktop through
        effects::background(drawlist, pos, pos + celosia::ui::size,
            base, ImColor(main.x, main.y, main.z, 0.55f), ImColor(accent.x, accent.y, accent.z, 0.45f), (float)style::general::rounding_window);
    }
    effects::noise(drawlist, pos, pos + celosia::ui::size, style::window::noise, (float)style::general::rounding_window);
    render::titlebar();
    render::sidebar();

    render::tab_content.drawlist = drawlist;
    render::tab_content.vtx_begin = drawlist->VtxBuffer.Size;
    if (ui::tab_current == "Widgets") // the tab being drawn, see ui::tab_current
        widgets_tab();
    else if (ui::tab_current == "Appearance")
        appearance_tab();
    render::tab_content.vtx_end = drawlist->VtxBuffer.Size;

    window::end();
}

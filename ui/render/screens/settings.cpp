#include "../../ui.h"
#include "screens.h"

// settings: the look, CelosUI's Appearance page from the demo. presets first, the picker and the sliders for going further,
// all of them only as far as style::limits lets them

static float color[4] = { celosia::style::general::main_color.Value.x, celosia::style::general::main_color.Value.y, celosia::style::general::main_color.Value.z, celosia::style::general::main_color.Value.w };

void fenrir::screens::settings() {
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

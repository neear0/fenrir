#include "../ui.h"

ImFont* celosia::resources::fonts::add(const std::string& name, const t_font& font) {
    // fonts only come from memory compiled into the binary, the font parser isn't safe to run on files someone else could have placed
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // static data, the atlas only reads it and must not free it
    config.FontBuilderFlags = font.hinting;
    ImFont* added = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(font.data), font.data_size, font.size * style::scale, &config);
    if (added)
        map[name] = added;
    return added;
}

ImFont* celosia::resources::fonts::get(const std::string& name) {
    const auto it = map.find(name);
    return it != map.end() ? it->second : ImGui::GetIO().Fonts->Fonts[0]; // the first font added is ImGui's default
}

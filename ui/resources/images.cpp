#include "../ui.h"

// png only and no file access: images come from memory compiled into the binary, like the fonts
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ASSERT(x) IM_ASSERT(x)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../external/stb/stb_image.h"

bool celosia::resources::images::add(const std::string& name, const unsigned char* png, int png_size) {
    int width, height;
    unsigned char* rgba = stbi_load_from_memory(png, png_size, &width, &height, nullptr, 4);
    if (!rgba)
        return false;
    const ImTextureID texture = platform::create_texture(rgba, width, height);
    stbi_image_free(rgba);
    if (!texture)
        return false;

    if (const auto it = map.find(name); it != map.end())
        platform::destroy_texture(it->second.texture);
    map[name] = { texture, ImVec2((float)width, (float)height) };
    return true;
}

const celosia::resources::images::t_image& celosia::resources::images::get(const std::string& name) {
    static const t_image none;
    const auto it = map.find(name);
    return it != map.end() ? it->second : none;
}

void celosia::resources::images::clear() {
    for (const auto& [name, image] : map)
        platform::destroy_texture(image.texture);
    map.clear();
}

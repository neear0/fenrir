// shared by the effect pixel shaders. each one is compiled twice from the same source:
// by fxc for d3d11, and by glslangValidator for vulkan, which defines VULKAN

#ifdef VULKAN
#define vk_location(n) [[vk::location(n)]]
#define vk_binding(n) [[vk::binding(n, 0)]]
#else
#define vk_location(n)
#define vk_binding(n)
#endif

struct ps_input { // what ImGui's vertex shader hands over, has to match it on both backends
    float4 pos : SV_POSITION; // pixel center in framebuffer pixels
    vk_location(0) float4 col : COLOR0; // white, every effect multiplies its alpha by col.a so a draw list's vertices can fade it
    vk_location(1) float2 uv : TEXCOORD0;
};

struct effect_constants { // matches celosia::effects::t_constants
    float4 reserved; // vulkan: ImGui's vertex shader keeps its scale and translation here, the push constants are shared
    float4 rect;     // min x, min y, max x, max y in framebuffer pixels
    float4 color_a;
    float4 color_b;
    float4 color_c;
    float4 params;   // x is always the corner rounding in pixels, the rest depends on the effect
};

#ifdef VULKAN
[[vk::push_constant]] cbuffer effect_block { effect_constants effect; };
#else
cbuffer effect_block : register(b0) { effect_constants effect; };
#endif

// whatever ImGui binds for the draw: the font atlas for most effects, the backdrop copy for blur
vk_binding(0) Texture2D source_texture : register(t0);
SamplerState source_sampler : register(s0);

float rounded_rect_distance(float2 p, float4 rect, float rounding) { // signed distance to the edge, negative inside
    float2 half_size = (rect.zw - rect.xy) * 0.5;
    float2 center = rect.xy + half_size;
    rounding = min(rounding, min(half_size.x, half_size.y));
    float2 q = abs(p - center) - half_size + rounding;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rounding;
}

float coverage(float distance) { // antialiased edge, one pixel wide
    return saturate(0.5 - distance);
}

float dither(float2 p) { // +-half a step of 8 bit color, breaks up banding in smooth gradients
    return (frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
}

float2 local_position(float2 p) { // 0 to 1 across the effect's rect
    return (p - effect.rect.xy) / max(effect.rect.zw - effect.rect.xy, 1.0);
}

// film grain over what's already drawn: lightens and darkens pixels at random, evenly in both directions so the area
// keeps its brightness. params: x rounding, y grain size in pixels, z strength (0 to 1), w seed (changes the pattern)

#include "effect.hlsli"

float hash(float2 cell, float seed) { // 0 to 1, integer hashing because sin() based hashes fall apart at large coordinates
    uint3 v = uint3(int3(int2(cell), int(seed)) + 0x8000);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    return float(v.x & 0xffffu) / 65535.0;
}

float value_noise(float2 p, float seed) { // smoothly blends between random values on a grid, grains bigger than a pixel stay soft
    float2 cell = floor(p);
    float2 f = p - cell;
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(cell, seed), b = hash(cell + float2(1, 0), seed);
    float c = hash(cell + float2(0, 1), seed), d = hash(cell + float2(1, 1), seed);
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 main(ps_input input) : SV_Target
{
    float2 p = input.pos.xy - effect.rect.xy; // the grain sticks to the rect, not the window
    float size = max(effect.params.y, 1.0);
    float n = size <= 1.0 ? hash(floor(p), effect.params.w) : value_noise(p / size, effect.params.w);
    n = n * 2.0 - 1.0;

    // white over the light grains, black over the dark ones
    float alpha = abs(n) * effect.params.z * coverage(rounded_rect_distance(input.pos.xy, effect.rect, effect.params.x)) * input.col.a;
    return float4(n > 0.0 ? float3(1, 1, 1) : float3(0, 0, 0), alpha);
}

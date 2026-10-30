// two color gradient with rounded corners, optionally sliding back and forth
// color_a -> color_b, params: x rounding, y angle in radians, z speed (0 = still), w time in seconds

#include "effect.hlsli"

float4 main(ps_input input) : SV_Target
{
    float2 direction = float2(cos(effect.params.y), sin(effect.params.y));
    float extent = abs(direction.x) + abs(direction.y); // how far the rect reaches along the direction, in 0-1 rect space
    float t = dot(local_position(input.pos.xy) - 0.5, direction) / extent + 0.5;
    t = 0.5 - 0.5 * cos(3.14159265 * (t + effect.params.w * effect.params.z)); // eased, and slides when animated

    float4 color = lerp(effect.color_a, effect.color_b, t);
    color.rgb += dither(input.pos.xy);
    color.a *= coverage(rounded_rect_distance(input.pos.xy, effect.rect, effect.params.x)) * input.col.a;
    return color;
}

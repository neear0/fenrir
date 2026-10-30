// soft glow around a rect, drawn on geometry that reaches params.y pixels past it. nothing is drawn inside the rect itself
// color_a, params: x rounding, y radius in pixels

#include "effect.hlsli"

float4 main(ps_input input) : SV_Target
{
    float distance = rounded_rect_distance(input.pos.xy, effect.rect, effect.params.x);
    float falloff = distance / max(effect.params.y, 1.0);
    float strength = exp(-4.0 * falloff * falloff) * saturate(distance + 0.5); // fades out by the radius, cut off inside
    return float4(effect.color_a.rgb, effect.color_a.a * strength * input.col.a);
}

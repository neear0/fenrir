// animated window background: soft blobs of color_b and color_c drifting over color_a (their alpha is how strong they are)
// params: x rounding, w time in seconds

#include "effect.hlsli"

float blob(float2 p, float2 center, float size) {
    float2 d = p - center;
    return exp(-dot(d, d) / (size * size));
}

float4 main(ps_input input) : SV_Target
{
    float2 size = max(effect.rect.zw - effect.rect.xy, 1.0);
    float aspect = size.x / size.y;
    float2 p = (input.pos.xy - effect.rect.xy) / size.y; // scaled by the height so the blobs stay round
    float t = effect.params.w * 0.15;

    float2 one = float2(aspect * (0.25 + 0.15 * sin(t * 1.3)), 0.30 + 0.20 * cos(t * 0.9));
    float2 two = float2(aspect * (0.75 + 0.15 * cos(t * 1.1)), 0.70 + 0.20 * sin(t * 1.7));
    float2 three = float2(aspect * (0.50 + 0.30 * sin(t * 0.7 + 2.0)), 0.50 + 0.30 * cos(t * 1.3 + 1.0));

    float3 color = effect.color_a.rgb;
    color = lerp(color, effect.color_b.rgb, blob(p, one, 0.45) * effect.color_b.a);
    color = lerp(color, effect.color_c.rgb, blob(p, two, 0.40) * effect.color_c.a);
    color = lerp(color, effect.color_b.rgb, blob(p, three, 0.30) * effect.color_b.a * 0.6);
    color += dither(input.pos.xy);

    return float4(color, effect.color_a.a * coverage(rounded_rect_distance(input.pos.xy, effect.rect, effect.params.x)) * input.col.a);
}

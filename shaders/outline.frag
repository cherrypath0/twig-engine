#version 450
// Post-process selection outline. Samples the selection mask; a pixel that is
// OUTSIDE the object but adjacent to it (within `thickness` texels) is drawn in
// the outline colour. This gives a uniform-width screen-space outline that does
// not depend on mesh geometry (unlike an inverted-hull).
layout(location = 0) out vec4 outColor;
layout(set = 2, binding = 0) uniform sampler2D u_mask;
layout(set = 3, binding = 0) uniform OutlineUBO {
    vec4 color;    // rgb = outline colour
    vec4 params;   // x = texelW, y = texelH, z = thickness(px), w = unused
} o;
void main() {
    vec2  texel = o.params.xy;
    float th    = max(o.params.z, 1.0);
    vec2  uv    = gl_FragCoord.xy * texel;
    float center = texture(u_mask, uv).r;
    float maxN = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            maxN = max(maxN, texture(u_mask, uv + vec2(float(x), float(y)) * texel * th).r);
        }
    float edge = (1.0 - step(0.5, center)) * step(0.5, maxN);
    if (edge < 0.5) discard;
    outColor = vec4(o.color.rgb, 1.0);
}

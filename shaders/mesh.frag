#version 450
// 3D mesh fragment shader. Source 2 style "material" inputs come in via the
// fragment uniform block. A base-colour texture is always bound (1x1 white when
// the material has none) so the pipeline layout is stable.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D g_tColor;

layout(set = 3, binding = 0) uniform FragUBO {
    vec4 baseColor;   // rgba   (g_vColorTint)
    vec4 lightDir;    // xyz = directional light dir, w = useTexture
    vec4 cameraPos;   // xyz = camera world position
    vec4 params;      // x = metallic, y = roughness, z = ambient, w = unused
} f;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(-f.lightDir.xyz);
    vec3 V = normalize(f.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    vec3 albedo = f.baseColor.rgb;
    if (f.lightDir.w > 0.5) albedo *= texture(g_tColor, vUV).rgb;

    float ndl     = max(dot(N, L), 0.0);
    float ambient = f.params.z;
    float rough   = clamp(f.params.y, 0.04, 1.0);
    float spec    = pow(max(dot(N, H), 0.0), mix(64.0, 4.0, rough)) * (1.0 - rough);

    vec3 color = albedo * (ambient + ndl) + vec3(spec) * ndl;
    outColor = vec4(color, f.baseColor.a);
}

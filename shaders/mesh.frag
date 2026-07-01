#version 450
// 3D mesh fragment shader. Supports three shading modes selected by the World:
//   0 = PBR   (Cook-Torrance GGX, metallic/roughness workflow)
//   1 = Phong (legacy half-lambert + blinn spec; also used by editor overlays)
//   2 = Unlit (albedo only)
// A base-colour texture is always bound (1x1 white when the material has none)
// so the pipeline layout is stable.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D g_tColor;

struct Light {
    vec4 position;   // xyz world pos, w = type (0 point,1 spot,2 dir,3 area)
    vec4 direction;  // xyz dir, w = range
    vec4 color;      // rgb, w = intensity
    vec4 params;     // x=cos(inner), y=cos(outer), z=areaW, w=areaH
};

layout(set = 3, binding = 0) uniform FragUBO {
    vec4  baseColor;    // rgba (g_vColorTint)
    vec4  lightDir;     // xyz = sun dir, w = useTexture
    vec4  cameraPos;    // xyz = camera world pos
    vec4  params;       // x = metallic, y = roughness, z = ambient, w = unused
    vec4  sunColor;     // rgb = sun color, w = intensity
    vec4  ambientColor; // rgb = ambient/sky color, w = intensity
    vec4  lightInfo;    // x = light count, y = mode, z = exposure, w = unused
    Light lights[16];
} f;

const float PI = 3.14159265359;

// --- Cook-Torrance GGX terms -------------------------------------------------
float distributionGGX(vec3 N, vec3 H, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d  = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
float geometrySchlick(float ndv, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return ndv / (ndv * (1.0 - k) + k);
}
float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlick(max(dot(N, V), 0.0), rough) *
           geometrySchlick(max(dot(N, L), 0.0), rough);
}
vec3 fresnelSchlick(float cosT, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// Distance attenuation with a smooth range cutoff (UE4-style).
float attenuate(float dist, float range) {
    float f2 = dist / max(range, 1e-3);
    f2 = f2 * f2;
    float win = clamp(1.0 - f2 * f2, 0.0, 1.0);
    return (win * win) / (dist * dist + 1.0);
}

void main() {
    vec3  N     = normalize(vNormal);
    vec3  V     = normalize(f.cameraPos.xyz - vWorldPos);
    int   mode  = int(f.lightInfo.y + 0.5);

    vec3 albedo = f.baseColor.rgb;
    if (f.lightDir.w > 0.5) albedo *= texture(g_tColor, vUV).rgb;

    // ---- Unlit ----
    if (mode == 2) {
        outColor = vec4(albedo, f.baseColor.a);
        return;
    }

    // ---- Phong (legacy; editor overlays rely on this exact formula) ----
    if (mode == 1) {
        vec3  L    = normalize(-f.lightDir.xyz);
        vec3  H    = normalize(L + V);
        float ndl  = max(dot(N, L), 0.0);
        float rgh  = clamp(f.params.y, 0.04, 1.0);
        float spec = pow(max(dot(N, H), 0.0), mix(64.0, 4.0, rgh)) * (1.0 - rgh);
        vec3  sunC = f.sunColor.rgb * f.sunColor.w;
        vec3  ambC = f.ambientColor.rgb * f.ambientColor.w;
        vec3  col  = albedo * (ambC + sunC * ndl) + sunC * spec * ndl;
        outColor = vec4(col, f.baseColor.a);
        return;
    }

    // ---- PBR (Cook-Torrance) ----
    float metal = clamp(f.params.x, 0.0, 1.0);
    float rough = clamp(f.params.y, 0.04, 1.0);
    vec3  F0    = mix(vec3(0.04), albedo, metal);
    vec3  diff  = albedo * (1.0 - metal);

    vec3 Lo = vec3(0.0);

    // Accumulate a single light's contribution.
    #define ADD_LIGHT(Lvec, radiance) {                             \
        vec3 L = (Lvec);                                            \
        vec3 H = normalize(L + V);                                  \
        float ndl = max(dot(N, L), 0.0);                           \
        if (ndl > 0.0) {                                            \
            float D = distributionGGX(N, H, rough);                \
            float G = geometrySmith(N, V, L, rough);               \
            vec3  Fr = fresnelSchlick(max(dot(H, V), 0.0), F0);    \
            vec3  spec = (D * G * Fr) /                             \
                max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);        \
            vec3  kd = (vec3(1.0) - Fr);                           \
            Lo += (kd * diff / PI + spec) * (radiance) * ndl;      \
        }                                                          \
    }

    // Directional sun (always present).
    {
        vec3 sunRad = f.sunColor.rgb * f.sunColor.w;
        ADD_LIGHT(normalize(-f.lightDir.xyz), sunRad);
    }

    // Punctual lights.
    int count = int(f.lightInfo.x + 0.5);
    for (int i = 0; i < count && i < 16; ++i) {
        Light lt = f.lights[i];
        int  type = int(lt.position.w + 0.5);
        vec3 radiance = lt.color.rgb * lt.color.w;
        vec3 L;
        if (type == 2) {                       // directional
            L = normalize(-lt.direction.xyz);
        } else {                               // point / spot / area
            vec3 toL = lt.position.xyz - vWorldPos;
            float dist = length(toL);
            L = toL / max(dist, 1e-4);
            radiance *= attenuate(dist, lt.direction.w);
            if (type == 1) {                   // spot cone falloff
                float cd = dot(normalize(-lt.direction.xyz), -L);
                float t  = clamp((cd - lt.params.y) / max(lt.params.x - lt.params.y, 1e-4), 0.0, 1.0);
                radiance *= t * t;
            } else if (type == 3) {            // area: soften by size (approx)
                radiance *= 1.0 + 0.15 * (lt.params.z + lt.params.w);
            }
        }
        ADD_LIGHT(L, radiance);
    }

    // Ambient/IBL-ish fill term.
    vec3 ambient = f.ambientColor.rgb * f.ambientColor.w * albedo;
    vec3 color   = ambient + Lo;

    // Exposure + Reinhard tonemap, then gamma.
    color = vec3(1.0) - exp(-color * max(f.lightInfo.z, 0.001));
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, f.baseColor.a);
}

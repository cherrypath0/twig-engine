#version 450
// Lit mesh fragment shader (used by the main scene pass). Identical to mesh.frag
// but adds directional sun shadow-map sampling. Editor overlays keep using the
// shorter mesh.frag; only this shader binds the shadow map (set 2, binding 1).

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D g_tColor;
layout(set = 2, binding = 1) uniform sampler2D g_tShadow;
layout(set = 2, binding = 2) uniform sampler2D g_tMR;      // roughness=G, metal=B
layout(set = 2, binding = 3) uniform sampler2D g_tNormal;  // tangent-space normal

struct Light {
    vec4 position;
    vec4 direction;
    vec4 color;
    vec4 params;
};

layout(set = 3, binding = 0) uniform FragUBO {
    vec4  baseColor;
    vec4  lightDir;
    vec4  cameraPos;
    vec4  params;
    vec4  sunColor;
    vec4  ambientColor;
    vec4  lightInfo;    // x = light count, y = mode, z = exposure
    Light lights[16];
    mat4  lightVP;      // world -> sun clip space
    vec4  shadowParams; // x = strength, y = enabled, z = texel size
} f;

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough; float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
float geometrySchlick(float ndv, float rough) {
    float r = rough + 1.0; float k = (r * r) / 8.0;
    return ndv / (ndv * (1.0 - k) + k);
}
float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlick(max(dot(N, V), 0.0), rough) *
           geometrySchlick(max(dot(N, L), 0.0), rough);
}
vec3 fresnelSchlick(float cosT, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}
float attenuate(float dist, float range) {
    float f2 = dist / max(range, 1e-3); f2 = f2 * f2;
    float win = clamp(1.0 - f2 * f2, 0.0, 1.0);
    return (win * win) / (dist * dist + 1.0);
}

// Perturb the geometric normal by a tangent-space normal map WITHOUT needing
// vertex tangents: build a cotangent frame from screen-space derivatives.
vec3 perturbNormal(vec3 N, vec2 uv, vec3 mapN) {
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 duv1 = dFdx(uv),       duv2 = dFdy(uv);
    vec3 T = dp1 * duv2.y - dp2 * duv1.y;
    T = normalize(T - N * dot(N, T));
    vec3 B = normalize(cross(N, T));
    return normalize(mat3(T, B, N) * mapN);
}

// Returns a [0,1] light multiplier for the sun: 1 = fully lit, <1 = shadowed.
float sunShadow(vec3 N, vec3 L) {
    if (f.shadowParams.y < 0.5) return 1.0;
    vec4 lp = f.lightVP * vec4(vWorldPos, 1.0);
    vec3 pc = lp.xyz / lp.w;
    vec2 uv = pc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;                       // texture origin flip
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || pc.z > 1.0)
        return 1.0;                          // outside the map -> lit
    float bias  = max(0.0025 * (1.0 - max(dot(N, L), 0.0)), 0.0008);
    float spread = f.shadowParams.z * max(f.shadowParams.w, 0.25);  // texel * softness
    int   R = clamp(int(f.lightInfo.w + 0.5), 1, 6);               // PCF kernel radius
    float sh = 0.0, cnt = 0.0;
    for (int x = -R; x <= R; ++x)
        for (int y = -R; y <= R; ++y) {
            float d = texture(g_tShadow, uv + vec2(x, y) * spread).r;
            sh += (pc.z - bias > d) ? 1.0 : 0.0;
            cnt += 1.0;
        }
    return 1.0 - (sh / cnt) * clamp(f.shadowParams.x, 0.0, 1.0);
}

void main() {
    vec3  N    = normalize(vNormal);
    vec3  V    = normalize(f.cameraPos.xyz - vWorldPos);
    int   mode = int(f.lightInfo.y + 0.5);

    vec3 albedo = f.baseColor.rgb;
    if (f.lightDir.w > 0.5) albedo *= texture(g_tColor, vUV).rgb;

    // Normal map (params.w) via derivative TBN — no vertex tangents required.
    if (f.params.w > 0.5) {
        vec3 mapN = texture(g_tNormal, vUV).xyz * 2.0 - 1.0;
        N = perturbNormal(N, vUV, mapN);
    }

    if (mode == 2) { outColor = vec4(albedo, f.baseColor.a); return; }

    vec3  Lsun = normalize(-f.lightDir.xyz);
    float shadow = sunShadow(N, Lsun);

    if (mode == 1) {                          // Phong
        vec3  H = normalize(Lsun + V);
        float ndl = max(dot(N, Lsun), 0.0);
        float rgh = clamp(f.params.y, 0.04, 1.0);
        float spec = pow(max(dot(N, H), 0.0), mix(64.0, 4.0, rgh)) * (1.0 - rgh);
        vec3  sunC = f.sunColor.rgb * f.sunColor.w * shadow;
        vec3  ambC = f.ambientColor.rgb * f.ambientColor.w;
        vec3  col = albedo * (ambC + sunC * ndl) + sunC * spec * ndl;
        outColor = vec4(col, f.baseColor.a);
        return;
    }

    // PBR — work in LINEAR space: the albedo texture/tint are authored in sRGB,
    // so linearize before lighting and re-encode with gamma at the end. (Without
    // this the output was double-brightened and looked washed out.)
    albedo = pow(albedo, vec3(2.2));
    // sample the metallic-roughness map (params.z) when present so the authored
    // per-texel metal/rough is used instead of the flat factors.
    float metal = clamp(f.params.x, 0.0, 1.0);
    float rough = clamp(f.params.y, 0.04, 1.0);
    if (f.params.z > 0.5) {
        vec4 mr = texture(g_tMR, vUV);
        rough = clamp(rough * mr.g, 0.04, 1.0);
        metal = clamp(metal * mr.b, 0.0, 1.0);
    }
    vec3  F0 = mix(vec3(0.04), albedo, metal);
    vec3  diff = albedo * (1.0 - metal);
    vec3  Lo = vec3(0.0);

    // Note: diffuse is NOT divided by PI here (artist-friendly intensities), so
    // a sun intensity of ~1 gives full Lambert shading rather than a dim result.
    #define ADD_LIGHT(Lvec, radiance) {                          \
        vec3 L = (Lvec); vec3 H = normalize(L + V);              \
        float ndl = max(dot(N, L), 0.0);                         \
        if (ndl > 0.0) {                                         \
            float D = distributionGGX(N, H, rough);             \
            float G = geometrySmith(N, V, L, rough);            \
            vec3 Fr = fresnelSchlick(max(dot(H, V), 0.0), F0);  \
            vec3 spec = (D * G * Fr) /                           \
                max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);     \
            spec = min(spec, vec3(4.0));   /* no blown highlights */ \
            vec3 kd = (vec3(1.0) - Fr);                          \
            Lo += (kd * diff + spec) * (radiance) * ndl;        \
        }                                                       \
    }

    { vec3 sunRad = f.sunColor.rgb * f.sunColor.w * shadow; ADD_LIGHT(Lsun, sunRad); }

    int count = int(f.lightInfo.x + 0.5);
    for (int i = 0; i < count && i < 16; ++i) {
        Light lt = f.lights[i];
        int type = int(lt.position.w + 0.5);
        vec3 radiance = lt.color.rgb * lt.color.w;
        vec3 L;
        if (type == 2) {
            L = normalize(-lt.direction.xyz);
        } else {
            vec3 toL = lt.position.xyz - vWorldPos;
            float dist = length(toL);
            L = toL / max(dist, 1e-4);
            radiance *= attenuate(dist, lt.direction.w);
            if (type == 1) {
                // cone axis = emission dir; -L is light->fragment. Same sign so
                // the lit cone matches the spot gizmo direction.
                float cd = dot(normalize(lt.direction.xyz), -L);
                float t = clamp((cd - lt.params.y) / max(lt.params.x - lt.params.y, 1e-4), 0.0, 1.0);
                radiance *= t * t;
            } else if (type == 3) {
                radiance *= 1.0 + 0.15 * (lt.params.z + lt.params.w);
            }
        }
        ADD_LIGHT(L, radiance);
    }

    vec3 ambient = f.ambientColor.rgb * f.ambientColor.w * albedo;
    vec3 color = ambient + Lo;
    color = vec3(1.0) - exp(-color * max(f.lightInfo.z, 0.001));
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, f.baseColor.a);
}

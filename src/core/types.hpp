#pragma once
// Common engine value types shared across subsystems.
#include "math.hpp"
#include <cstdint>

struct Vertex {
    em::vec3 pos    {0, 0, 0};
    em::vec3 normal {0, 1, 0};
    em::vec2 uv     {0, 0};
};

// Matches shaders/mesh.vert VertexUBO (std140-friendly: two mat4s).
struct MeshVertexUBO {
    em::mat4 mvp;
    em::mat4 model;
};

// A single punctual light, std140-packed (4 vec4 = 64 bytes). Mirrors the
// GLSL Light struct in mesh.frag.
struct GpuLight {
    em::vec4 position  {0, 0, 0, 0};  // xyz world pos, w = type (0 point,1 spot,2 dir,3 area)
    em::vec4 direction {0, -1, 0, 0}; // xyz normalized dir, w = range
    em::vec4 color     {0, 0, 0, 0};  // rgb color, w = intensity
    em::vec4 params    {0, 0, 0, 0};  // x=cos(inner), y=cos(outer), z=areaW, w=areaH
};

static constexpr int kMaxLights = 16;

// Matches shaders/mesh.frag FragUBO.
struct MeshFragUBO {
    em::vec4 baseColor;
    em::vec4 lightDir;      // xyz sun dir, w = useTexture
    em::vec4 cameraPos;     // xyz
    em::vec4 params;        // metallic, roughness, ambient, _
    em::vec4 sunColor {1, 1, 1, 1};      // rgb = sun color, w = intensity
    em::vec4 ambientColor {1, 1, 1, 1};  // rgb = ambient/sky color, w = intensity
    // x = active light count, y = shading mode (0 PBR,1 Phong,2 Unlit),
    // z = exposure, w = unused. Defaults keep overlay draws (Phong, no lights,
    // exposure 1) rendering exactly as before.
    em::vec4 lightInfo {0, 1, 1, 0};
    GpuLight lights[kMaxLights];
    // Directional shadow map lookup (only read by the lit mesh shader; overlay
    // shaders declare a shorter block and ignore these trailing fields).
    em::mat4 lightVP;                       // world -> sun light clip space
    em::vec4 shadowParams {0, 0, 0, 0};     // x=strength, y=enabled, z=texel size, w=_
};

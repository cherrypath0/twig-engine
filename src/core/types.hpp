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

// Matches shaders/mesh.frag FragUBO.
struct MeshFragUBO {
    em::vec4 baseColor;
    em::vec4 lightDir;   // xyz dir, w = useTexture
    em::vec4 cameraPos;  // xyz
    em::vec4 params;     // metallic, roughness, ambient, _
};

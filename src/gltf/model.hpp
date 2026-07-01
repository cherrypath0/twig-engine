#pragma once
// GLB / glTF model loading (via cgltf) and procedural primitives.
#include "core/types.hpp"
#include <string>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUBuffer;

// CPU-side geometry.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    em::vec3 bounds_min {0, 0, 0};
    em::vec3 bounds_max {0, 0, 0};
    void recompute_bounds();
};

// One drawable chunk out of a glTF file (a single primitive).
struct Primitive {
    MeshData    mesh;
    std::string material_name;       // glTF material name, if any
    em::vec4    base_color {1, 1, 1, 1};
    float       metalness = 0.0f;    // glTF metallic factor
    float       roughness = 1.0f;    // glTF roughness factor
    std::vector<uint8_t> tex_rgba;   // decoded base-colour texture (RGBA), empty if none
    int         tex_w = 0, tex_h = 0;
    std::string tex_key;             // dedup key (glTF image index)
    // Metallic-roughness map (glTF packs roughness in G, metalness in B) and a
    // tangent-space normal map. Empty when the material has none.
    std::vector<uint8_t> mr_rgba;   int mr_w = 0, mr_h = 0;
    std::vector<uint8_t> nrm_rgba;  int nrm_w = 0, nrm_h = 0;
};

struct Model {
    std::vector<Primitive> primitives;
    std::string source_path;
};

// GPU-resident mesh.
struct GpuMesh {
    SDL_GPUBuffer* vbo = nullptr;
    SDL_GPUBuffer* ibo = nullptr;
    uint32_t index_count = 0;
    em::vec3 bounds_min {0, 0, 0};
    em::vec3 bounds_max {0, 0, 0};
};

namespace model {

// Load a .glb (or .gltf) file. Node transforms are baked into vertices.
bool load_glb(const std::string& path, Model& out);

// Procedural primitives (unit-ish, centered on origin).
MeshData make_cube(float half = 0.5f);
MeshData make_plane(float half = 10.0f, float uv_tiles = 10.0f);
MeshData make_sphere(float radius = 0.5f, int segments = 24);

// Upload CPU geometry to the GPU.
GpuMesh  upload(SDL_GPUDevice* dev, const MeshData& data);
void     destroy(SDL_GPUDevice* dev, GpuMesh& mesh);

} // namespace model

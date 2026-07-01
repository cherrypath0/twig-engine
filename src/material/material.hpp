#pragma once
// Custom Source 2-inspired material format ( .tgmat ).
//
//   Material
//   {
//       shader        "mesh"
//       g_tColor      "assets/textures/crate.png"
//       g_vColorTint  [1.0 0.6 0.2 1.0]
//       g_flMetalness 0.1
//       g_flRoughness 0.5
//       g_flAmbient   0.15
//   }
//
// Keys accept Source 2 style names (g_tColor, g_vColorTint, g_flRoughness ...)
// or friendly aliases (color, tint, roughness ...). See material.cpp.
#include "math.hpp"
#include <string>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

struct Material {
    std::string name      = "unnamed";
    std::string shader    = "mesh";          // -> shaders/<shader>.vert/.frag
    std::string color_texture;               // optional path
    em::vec4    color_tint {1, 1, 1, 1};
    float       metalness = 0.0f;
    float       roughness = 0.8f;
    float       ambient   = 0.15f;

    // Resolved GPU state (filled by resolve_gpu).
    SDL_GPUTexture* tex = nullptr;
    bool use_texture = false;
    // Extra PBR maps (set by the GLB importer). tex_mr packs roughness in G and
    // metalness in B (glTF convention); tex_normal is a tangent-space normal map.
    SDL_GPUTexture* tex_mr = nullptr;      bool use_mr = false;
    SDL_GPUTexture* tex_normal = nullptr;  bool use_normal = false;
};

namespace material {

Material make_default();

// Parse a .tgmat file. Returns make_default() values for anything not specified.
bool load_tgmat(const std::string& path, Material& out);

// Load color_texture (via stb_image) into a GPU texture, or fall back to
// `white_fallback`. Safe to call once after parsing.
void resolve_gpu(SDL_GPUDevice* dev, Material& mat, SDL_GPUTexture* white_fallback);

} // namespace material

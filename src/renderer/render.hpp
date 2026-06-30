#pragma once
// Forward-rendering of the 3D scene (mesh + material + depth) followed by the
// Nuklear UI overlay, all through the SDL3 GPU API.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "gltf/model.hpp"   // GpuMesh (gizmo handle meshes)

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct Scene;
struct Camera;
struct Material;
class  NuklearBackend;

class Renderer {
public:
    bool init(SDL_GPUDevice* dev, SDL_Window* window);
    void shutdown();

    // Render one full frame. Returns false only on a fatal GPU error.
    bool render(Scene& scene, Camera& cam,
                const std::unordered_map<std::string, Material>& materials,
                NuklearBackend& ui,
                float vp_x, float vp_y, float vp_w, float vp_h,
                int selected, int gizmo_mode, bool gizmo_local, int gizmo_hot, bool show_colliders,
                const std::vector<int>& highlight);

    SDL_GPUDevice*  device() const { return dev_; }
    uint32_t        swapchain_format() const { return swap_fmt_; }
    SDL_GPUTexture* white() const { return white_; }
    SDL_GPUSampler* sampler() const { return sampler_; }

private:
    void ensure_depth(uint32_t w, uint32_t h);
    void ensure_mask(uint32_t w, uint32_t h);

    SDL_GPUDevice*  dev_    = nullptr;
    SDL_Window*     window_ = nullptr;
    SDL_GPUGraphicsPipeline* mesh_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* mask_pipeline_ = nullptr;          // selected -> silhouette mask
    SDL_GPUGraphicsPipeline* outline_post_pipeline_ = nullptr;  // mask -> screen-space outline
    SDL_GPUTexture* mask_   = nullptr;
    uint32_t mask_w_ = 0, mask_h_ = 0;
    // 3D transform gizmo handles (always-on-top, screen-constant size)
    SDL_GPUGraphicsPipeline* gizmo_pipeline_ = nullptr;
    GpuMesh gizmo_arrow_{};   // move
    GpuMesh gizmo_scale_{};   // scale (box-tipped)
    GpuMesh gizmo_ring_{};    // rotate
    // Collider debug visualization (transparent faces + clean edge/ring lines)
    SDL_GPUGraphicsPipeline* collider_fill_pipeline_ = nullptr;   // triangles, faint faces
    SDL_GPUGraphicsPipeline* collider_line_pipeline_ = nullptr;   // FILLMODE_LINE (convex mesh)
    SDL_GPUGraphicsPipeline* collider_edge_pipeline_ = nullptr;   // LINELIST (box edges / rings)
    GpuMesh collider_cube_{};        // unit cube (faint faces)
    GpuMesh collider_box_edges_{};   // unit cube, 12 edges as a line list
    GpuMesh collider_ring_{};        // unit circle (line list) for sphere/capsule
    GpuMesh gizmo_frustum_{};        // camera frustum wireframe (line list)
    SDL_GPUTexture* depth_  = nullptr;
    SDL_GPUTexture* white_  = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    uint32_t depth_w_ = 0, depth_h_ = 0;
    uint32_t swap_fmt_ = 0;
};

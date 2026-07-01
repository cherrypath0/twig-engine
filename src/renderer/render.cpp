#include "pch.hpp"
#include "math.hpp"
#include "renderer/render.hpp"
#include "core/types.hpp"
#include "scene/scene.hpp"
#include "camera/camera.hpp"
#include "material/material.hpp"
#include "gpu/gpu.hpp"
#include "ui/nuklear_backend.hpp"
#include "stb_image_write.h"   // stbi_write_png (impl lives in thirdparty.cpp)

#include <cstddef>
#include <cmath>
#include <cstring>
#include <vector>

static constexpr SDL_GPUTextureFormat DEPTH_FORMAT = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

// ---- procedural gizmo handle meshes (unit shapes along +Y / +Z) -------------
namespace {
constexpr float kTau = 6.28318530718f;
void g_cyl(MeshData& m, float r, float y0, float y1, int seg) {
    uint32_t b = (uint32_t)m.vertices.size();
    for (int i = 0; i <= seg; ++i) {
        float a = (float)i / seg * kTau, c = std::cos(a), s = std::sin(a);
        em::vec3 n{c, 0, s};
        m.vertices.push_back({{c * r, y0, s * r}, n, {0, 0}});
        m.vertices.push_back({{c * r, y1, s * r}, n, {0, 1}});
    }
    for (int i = 0; i < seg; ++i) {
        uint32_t a = b + i * 2; uint32_t q[6] = {a, a + 1, a + 2, a + 2, a + 1, a + 3};
        m.indices.insert(m.indices.end(), q, q + 6);
    }
}
void g_cone(MeshData& m, float r, float y0, float y1, int seg) {
    uint32_t b = (uint32_t)m.vertices.size();
    for (int i = 0; i <= seg; ++i) {
        float a = (float)i / seg * kTau, c = std::cos(a), s = std::sin(a);
        em::vec3 n = em::normalize(em::vec3{c, 0.5f, s});
        m.vertices.push_back({{c * r, y0, s * r}, n, {0, 0}});
        m.vertices.push_back({{0, y1, 0}, n, {0, 1}});
    }
    for (int i = 0; i < seg; ++i) {
        uint32_t a = b + i * 2; uint32_t q[3] = {a, a + 1, a + 2};
        m.indices.insert(m.indices.end(), q, q + 3);
    }
}
void g_box(MeshData& m, em::vec3 c, em::vec3 h) {
    MeshData cube = model::make_cube(1.0f);
    uint32_t base = (uint32_t)m.vertices.size();
    for (Vertex v : cube.vertices) {
        v.pos = {c.x + v.pos.x * h.x, c.y + v.pos.y * h.y, c.z + v.pos.z * h.z};
        m.vertices.push_back(v);
    }
    for (uint32_t i : cube.indices) m.indices.push_back(base + i);
}
void g_torus(MeshData& m, float R, float tube, int seg, int side) {
    uint32_t base = (uint32_t)m.vertices.size();
    for (int i = 0; i <= seg; ++i) {
        float u = (float)i / seg * kTau, cu = std::cos(u), su = std::sin(u);
        for (int j = 0; j <= side; ++j) {
            float v = (float)j / side * kTau, cv = std::cos(v), sv = std::sin(v);
            m.vertices.push_back({{(R + tube * cv) * cu, (R + tube * cv) * su, tube * sv},
                                  {cv * cu, cv * su, sv}, {0, 0}});
        }
    }
    int stride = side + 1;
    for (int i = 0; i < seg; ++i) for (int j = 0; j < side; ++j) {
        uint32_t a = base + i * stride + j, b = a + stride;
        uint32_t q[6] = {a, b, a + 1, a + 1, b, b + 1};
        m.indices.insert(m.indices.end(), q, q + 6);
    }
}
// Unit cube (-0.5..0.5) as 12 clean edges, drawn as a LINE LIST (no diagonals).
void g_box_edges(MeshData& m) {
    const em::vec3 c[8] = {
        {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
        {-0.5f,-0.5f, 0.5f},{0.5f,-0.5f, 0.5f},{0.5f,0.5f, 0.5f},{-0.5f,0.5f, 0.5f}};
    for (const em::vec3& p : c) m.vertices.push_back({p, {0, 1, 0}, {0, 0}});
    const int e[24] = {0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7};
    for (int i : e) m.indices.push_back((uint32_t)i);
}
// Unit circle (radius 1, XY plane) as a LINE LIST loop. Oriented per-axis at
// draw time to fake a sphere/capsule with three great-circle rings.
void g_ring_line(MeshData& m, int seg) {
    for (int i = 0; i < seg; ++i) {
        float a = (float)i / seg * kTau;
        m.vertices.push_back({{std::cos(a), std::sin(a), 0.0f}, {0, 0, 1}, {0, 0}});
    }
    for (int i = 0; i < seg; ++i) { m.indices.push_back(i); m.indices.push_back((i + 1) % seg); }
}
void g_frustum(MeshData& m) {   // camera frustum wireframe (apex -> -Z), LINE LIST
    const float w = 0.4f, h = 0.3f, d = -1.0f;
    em::vec3 a{0, 0, 0};
    em::vec3 c[4] = {{-w, -h, d}, {w, -h, d}, {w, h, d}, {-w, h, d}};
    auto line = [&](em::vec3 p, em::vec3 q) {
        uint32_t b = (uint32_t)m.vertices.size();
        m.vertices.push_back({p, {0, 1, 0}, {0, 0}});
        m.vertices.push_back({q, {0, 1, 0}, {0, 0}});
        m.indices.push_back(b); m.indices.push_back(b + 1);
    };
    for (int i = 0; i < 4; ++i) line(a, c[i]);
    for (int i = 0; i < 4; ++i) line(c[i], c[(i + 1) % 4]);
}
// Light gizmo line meshes. All emit along local -Y (matching the light dir
// convention), so the entity's world matrix orients them automatically.
static void g_line(MeshData& m, em::vec3 p, em::vec3 q) {
    uint32_t b = (uint32_t)m.vertices.size();
    m.vertices.push_back({p, {0, 1, 0}, {0, 0}});
    m.vertices.push_back({q, {0, 1, 0}, {0, 0}});
    m.indices.push_back(b); m.indices.push_back(b + 1);
}
void g_light_cone(MeshData& m) {          // spot: apex at origin, base ring at y=-1 r=1
    const int seg = 24;
    for (int i = 0; i < seg; ++i) {
        float a0 = (float)i / seg * kTau, a1 = (float)(i + 1) / seg * kTau;
        g_line(m, {std::cos(a0), -1.0f, std::sin(a0)}, {std::cos(a1), -1.0f, std::sin(a1)});
    }
    for (int i = 0; i < 4; ++i) { float a = (float)i / 4 * kTau; g_line(m, {0, 0, 0}, {std::cos(a), -1.0f, std::sin(a)}); }
}
void g_light_arrow(MeshData& m) {         // directional: parallel rays + arrowhead
    for (int i = -1; i <= 1; ++i) { float o = (float)i * 0.35f; g_line(m, {o, 0.6f, 0}, {o, -0.8f, 0}); }
    g_line(m, {0, -0.8f, 0}, {0.18f, -0.45f, 0});  g_line(m, {0, -0.8f, 0}, {-0.18f, -0.45f, 0});
    g_line(m, {0, -0.8f, 0}, {0, -0.45f, 0.18f});  g_line(m, {0, -0.8f, 0}, {0, -0.45f, -0.18f});
}
void g_light_quad(MeshData& m) {          // area: unit rectangle in XZ + direction stub
    em::vec3 c[4] = {{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f}};
    for (int i = 0; i < 4; ++i) g_line(m, c[i], c[(i + 1) % 4]);
    g_line(m, {0, 0, 0}, {0, -0.6f, 0});
}
// Column-major model matrix from a scaled orthonormal basis + translation.
em::mat4 g_model(em::vec3 x, em::vec3 y, em::vec3 z, em::vec3 t, float s) {
    em::mat4 m{};
    m.m[0][0] = x.x * s; m.m[0][1] = x.y * s; m.m[0][2] = x.z * s; m.m[0][3] = 0;
    m.m[1][0] = y.x * s; m.m[1][1] = y.y * s; m.m[1][2] = y.z * s; m.m[1][3] = 0;
    m.m[2][0] = z.x * s; m.m[2][1] = z.y * s; m.m[2][2] = z.z * s; m.m[2][3] = 0;
    m.m[3][0] = t.x; m.m[3][1] = t.y; m.m[3][2] = t.z; m.m[3][3] = 1;
    return m;
}
} // namespace

bool Renderer::init(SDL_GPUDevice* dev, SDL_Window* window) {
    dev_ = dev;
    window_ = window;
    swap_fmt_ = SDL_GetGPUSwapchainTextureFormat(dev, window);

    white_   = gpu::create_white_texture(dev);
    sampler_ = gpu::create_linear_sampler(dev);

    SDL_GPUShader* vs = gpu::load_spirv(dev, "shaders/mesh.vert.spv", 0, 0, 1);
    SDL_GPUShader* fs = gpu::load_spirv(dev, "shaders/mesh.frag.spv", 1, 1, 1);
    SDL_GPUShader* fs_lit = gpu::load_spirv(dev, "shaders/mesh_lit.frag.spv", 1, 4, 1);
    SDL_GPUShader* shadow_fs = gpu::load_spirv(dev, "shaders/shadow.frag.spv", 1, 0, 0);
    if (!vs || !fs || !fs_lit || !shadow_fs) { warnln("Renderer: mesh shaders failed to load"); return false; }

    SDL_GPUVertexBufferDescription vb{};
    vb.slot = 0;
    vb.pitch = sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[3]{};
    attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[0].offset = offsetof(Vertex, pos);
    attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; attrs[2].offset = offsetof(Vertex, uv);

    SDL_GPUColorTargetDescription color_desc{};
    color_desc.format = static_cast<SDL_GPUTextureFormat>(swap_fmt_);

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vs;
    pci.fragment_shader = fs_lit;   // main scene pass = lit + shadowed shader
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vb;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = attrs;
    pci.vertex_input_state.num_vertex_attributes = 3;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pci.depth_stencil_state.enable_depth_test = true;
    pci.depth_stencil_state.enable_depth_write = true;
    pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    pci.target_info.color_target_descriptions = &color_desc;
    pci.target_info.num_color_targets = 1;
    pci.target_info.has_depth_stencil_target = true;
    pci.target_info.depth_stencil_format = DEPTH_FORMAT;

    mesh_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &pci);

    // Overlays (gizmo + colliders) use the simpler 1-sampler mesh.frag so they
    // never need the shadow map bound.
    pci.fragment_shader = fs;

    // Gizmo handles reuse the mesh pipeline settings (depth test ON so they
    // self-occlude correctly); "always on top" comes from CLEARING the depth
    // buffer in the gizmo pass so the handles ignore the scene's depth.
    gizmo_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &pci);

    // Collider debug: transparent green faces (blend) + wireframe edges, always
    // visible (no depth) so colliders read as an X-ray overlay.
    SDL_GPUColorTargetDescription cdesc{};
    cdesc.format = static_cast<SDL_GPUTextureFormat>(swap_fmt_);
    cdesc.blend_state.enable_blend = true;
    cdesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    cdesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    cdesc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    cdesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    cdesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    cdesc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo cp = pci;
    cp.target_info.color_target_descriptions = &cdesc;
    cp.target_info.has_depth_stencil_target = false;
    cp.depth_stencil_state.enable_depth_test = false;
    cp.depth_stencil_state.enable_depth_write = false;
    cp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    collider_fill_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &cp);
    cp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
    collider_line_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &cp);
    cp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    cp.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;   // clean edges / rings
    collider_edge_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &cp);

    // Depth-tested variants: same look, but occluded by scene geometry (depth
    // test ON, no depth write) so colliders can be drawn "behind" objects.
    SDL_GPUGraphicsPipelineCreateInfo cpd = cp;   // currently LINELIST
    cpd.target_info.has_depth_stencil_target = true;
    cpd.target_info.depth_stencil_format = DEPTH_FORMAT;
    cpd.depth_stencil_state.enable_depth_test = true;
    cpd.depth_stencil_state.enable_depth_write = false;
    cpd.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    collider_edge_depth_ = SDL_CreateGPUGraphicsPipeline(dev, &cpd);
    cpd.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    cpd.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    collider_fill_depth_ = SDL_CreateGPUGraphicsPipeline(dev, &cpd);
    cpd.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
    collider_line_depth_ = SDL_CreateGPUGraphicsPipeline(dev, &cpd);

    // Sun shadow map: depth-only pass rendered from the light's point of view.
    {
        SDL_GPUGraphicsPipelineCreateInfo sp{};
        sp.vertex_shader = vs;
        sp.fragment_shader = shadow_fs;
        sp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        sp.vertex_input_state.vertex_buffer_descriptions = &vb;
        sp.vertex_input_state.num_vertex_buffers = 1;
        sp.vertex_input_state.vertex_attributes = attrs;
        sp.vertex_input_state.num_vertex_attributes = 3;
        sp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        sp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        sp.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        sp.rasterizer_state.enable_depth_bias = true;         // combat shadow acne
        sp.rasterizer_state.depth_bias_constant_factor = 1.25f;
        sp.rasterizer_state.depth_bias_slope_factor = 2.5f;
        sp.depth_stencil_state.enable_depth_test = true;
        sp.depth_stencil_state.enable_depth_write = true;
        sp.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        sp.target_info.num_color_targets = 0;
        sp.target_info.has_depth_stencil_target = true;
        sp.target_info.depth_stencil_format = DEPTH_FORMAT;
        shadow_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &sp);

        shadow_dim_ = 2048;
        SDL_GPUTextureCreateInfo sci{};
        sci.type = SDL_GPU_TEXTURETYPE_2D;
        sci.format = DEPTH_FORMAT;
        sci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        sci.width = shadow_dim_; sci.height = shadow_dim_;
        sci.layer_count_or_depth = 1; sci.num_levels = 1;
        shadow_map_ = SDL_CreateGPUTexture(dev, &sci);
        if (shadow_map_) SDL_SetGPUTextureName(dev, shadow_map_, "sun.shadow");
    }

    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    SDL_ReleaseGPUShader(dev, fs_lit);
    SDL_ReleaseGPUShader(dev, shadow_fs);
    if (!mesh_pipeline_ || !gizmo_pipeline_) { warnln("Renderer: pipeline failed: %s", SDL_GetError()); return false; }

    // Build the procedural gizmo handle meshes once.
    {
        MeshData arw; g_cyl(arw, 0.018f, 0.0f, 0.82f, 14); g_cone(arw, 0.06f, 0.78f, 1.0f, 14);
        arw.recompute_bounds(); gizmo_arrow_ = model::upload(dev, arw);
        MeshData scl; g_cyl(scl, 0.018f, 0.0f, 0.86f, 14); g_box(scl, {0, 0.93f, 0}, {0.07f, 0.07f, 0.07f});
        scl.recompute_bounds(); gizmo_scale_ = model::upload(dev, scl);
        MeshData rng; g_torus(rng, 0.9f, 0.022f, 56, 8);
        rng.recompute_bounds(); gizmo_ring_ = model::upload(dev, rng);
        MeshData cb = model::make_cube(0.5f); cb.recompute_bounds(); collider_cube_ = model::upload(dev, cb);
        MeshData be; g_box_edges(be); be.recompute_bounds(); collider_box_edges_ = model::upload(dev, be);
        MeshData rl; g_ring_line(rl, 48); rl.recompute_bounds(); collider_ring_ = model::upload(dev, rl);
        MeshData fr; g_frustum(fr); fr.recompute_bounds(); gizmo_frustum_ = model::upload(dev, fr);
        MeshData lc; g_light_cone(lc);  lc.recompute_bounds(); light_cone_ = model::upload(dev, lc);
        MeshData la; g_light_arrow(la); la.recompute_bounds(); light_dir_  = model::upload(dev, la);
        MeshData lq; g_light_quad(lq);  lq.recompute_bounds(); light_quad_ = model::upload(dev, lq);
    }

    // ---- selection outline: post-process edge detection -------------------
    // (a) render the selected object's silhouette as solid white into a mask.
    SDL_GPUShader* mvs = gpu::load_spirv(dev, "shaders/mesh.vert.spv", 0, 0, 1);
    SDL_GPUShader* mfs = gpu::load_spirv(dev, "shaders/mask.frag.spv", 1, 0, 0);
    if (mvs && mfs) {
        SDL_GPUColorTargetDescription mdesc{};
        mdesc.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo mp{};
        mp.vertex_shader = mvs; mp.fragment_shader = mfs;
        mp.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        mp.vertex_input_state.vertex_buffer_descriptions = &vb;
        mp.vertex_input_state.num_vertex_buffers = 1;
        mp.vertex_input_state.vertex_attributes = attrs;
        mp.vertex_input_state.num_vertex_attributes = 3;
        mp.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        mp.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        mp.target_info.color_target_descriptions = &mdesc;
        mp.target_info.num_color_targets = 1;
        mask_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &mp);
    }
    if (mvs) SDL_ReleaseGPUShader(dev, mvs);
    if (mfs) SDL_ReleaseGPUShader(dev, mfs);

    // (b) full-screen pass: edge-detect the mask and composite the outline.
    SDL_GPUShader* fvs = gpu::load_spirv(dev, "shaders/fullscreen.vert.spv", 0, 0, 0);
    SDL_GPUShader* ofs = gpu::load_spirv(dev, "shaders/outline.frag.spv", 1, 1, 1);
    if (fvs && ofs) {
        SDL_GPUColorTargetDescription odesc{};
        odesc.format = static_cast<SDL_GPUTextureFormat>(swap_fmt_);
        odesc.blend_state.enable_blend = true;
        odesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        odesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        odesc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        odesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        odesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        odesc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo op{};
        op.vertex_shader = fvs; op.fragment_shader = ofs;
        op.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        op.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        op.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        op.target_info.color_target_descriptions = &odesc;
        op.target_info.num_color_targets = 1;
        outline_post_pipeline_ = SDL_CreateGPUGraphicsPipeline(dev, &op);
    }
    if (fvs) SDL_ReleaseGPUShader(dev, fvs);
    if (ofs) SDL_ReleaseGPUShader(dev, ofs);
    if (!mask_pipeline_ || !outline_post_pipeline_)
        warnln("Renderer: selection-outline pipelines unavailable (outline disabled)");

    println("Renderer ready (swapchain format %u)", swap_fmt_);
    return true;
}

void Renderer::shutdown() {
    if (depth_)         SDL_ReleaseGPUTexture(dev_, depth_);
    if (white_)         SDL_ReleaseGPUTexture(dev_, white_);
    if (sampler_)       SDL_ReleaseGPUSampler(dev_, sampler_);
    if (mesh_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, mesh_pipeline_);
    if (mask_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, mask_pipeline_);
    if (outline_post_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, outline_post_pipeline_);
    if (gizmo_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, gizmo_pipeline_);
    if (collider_fill_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_fill_pipeline_);
    if (collider_line_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_line_pipeline_);
    if (collider_edge_pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_edge_pipeline_);
    if (collider_fill_depth_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_fill_depth_);
    if (collider_line_depth_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_line_depth_);
    if (collider_edge_depth_) SDL_ReleaseGPUGraphicsPipeline(dev_, collider_edge_depth_);
    model::destroy(dev_, gizmo_arrow_);
    model::destroy(dev_, gizmo_scale_);
    model::destroy(dev_, gizmo_ring_);
    model::destroy(dev_, collider_cube_);
    model::destroy(dev_, collider_box_edges_);
    model::destroy(dev_, collider_ring_);
    model::destroy(dev_, gizmo_frustum_);
    if (mask_) SDL_ReleaseGPUTexture(dev_, mask_);
    depth_ = white_ = mask_ = nullptr; sampler_ = nullptr;
    mesh_pipeline_ = mask_pipeline_ = outline_post_pipeline_ = nullptr;
}

void Renderer::ensure_mask(uint32_t w, uint32_t h) {
    if (mask_ && w == mask_w_ && h == mask_h_) return;
    if (mask_) SDL_ReleaseGPUTexture(dev_, mask_);
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = w; tci.height = h;
    tci.layer_count_or_depth = 1; tci.num_levels = 1;
    mask_ = SDL_CreateGPUTexture(dev_, &tci);
    mask_w_ = w; mask_h_ = h;
}

void Renderer::ensure_depth(uint32_t w, uint32_t h) {
    if (depth_ && w == depth_w_ && h == depth_h_) return;
    if (depth_) SDL_ReleaseGPUTexture(dev_, depth_);

    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = DEPTH_FORMAT;
    tci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    tci.width = w; tci.height = h;
    tci.layer_count_or_depth = 1; tci.num_levels = 1;
    depth_ = SDL_CreateGPUTexture(dev_, &tci);
    depth_w_ = w; depth_h_ = h;
}

static const Material* lookup(const std::unordered_map<std::string, Material>& m, const std::string& key) {
    auto it = m.find(key);
    if (it != m.end()) return &it->second;
    it = m.find("default");
    return it != m.end() ? &it->second : nullptr;
}

bool Renderer::render(Scene& scene, Camera& cam,
                      const std::unordered_map<std::string, Material>& materials,
                      NuklearBackend& ui,
                      float vp_x, float vp_y, float vp_w, float vp_h,
                      int selected, int gizmo_mode, bool gizmo_local, int gizmo_hot, bool show_colliders,
                      bool colliders_xray, const std::vector<int>& highlight) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    if (!cmd) { warnln("AcquireGPUCommandBuffer: %s", SDL_GetError()); return false; }

    SDL_GPUTexture* swap = nullptr;
    Uint32 w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swap, &w, &h)) {
        warnln("AcquireSwapchain: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        return true;
    }
    if (!swap || w == 0 || h == 0) { SDL_SubmitGPUCommandBuffer(cmd); return true; }

    // When a screenshot is pending, render the whole frame into a downloadable
    // offscreen target (swapchain textures can't be downloaded), then blit it to
    // the window afterwards. `target` is where every color pass draws this frame.
    SDL_GPUTexture* target = swap;
    const bool capturing = !screenshot_path_.empty();
    if (capturing) {
        if (!capture_tex_ || cap_w_ != w || cap_h_ != h) {
            if (capture_tex_) SDL_ReleaseGPUTexture(dev_, capture_tex_);
            SDL_GPUTextureCreateInfo ci{};
            ci.type = SDL_GPU_TEXTURETYPE_2D;
            ci.format = (SDL_GPUTextureFormat)swap_fmt_;
            ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ci.width = w; ci.height = h; ci.layer_count_or_depth = 1; ci.num_levels = 1;
            capture_tex_ = SDL_CreateGPUTexture(dev_, &ci);
            cap_w_ = w; cap_h_ = h;
        }
        if (capture_tex_) target = capture_tex_;
    }

    // Upload UI geometry (copy pass) before any render pass begins.
    ui.prepare(cmd);
    ensure_depth(w, h);

    if (vp_w <= 0.0f || vp_h <= 0.0f) { vp_x = 0.0f; vp_y = 0.0f; vp_w = (float)w; vp_h = (float)h; }
    const float aspect = vp_w / vp_h;
    const em::mat4 view = cam.view();
    const em::mat4 proj = cam.proj(aspect);

    // ---- Sun shadow map: render scene depth from the directional light ----
    // Ortho frustum centered on the camera focus so the shadowed area follows
    // the view. Standard directional-shadow setup.
    em::mat4 lightVP = em::mat4::identity();
    bool shadows_on = scene.sun_shadows && shadow_pipeline_ && shadow_map_;
    if (shadows_on) {
        // A visible Directional light with "Cast Shadows" overrides the sun dir.
        em::vec3 sd = em::normalize(scene.sun_dir);
        for (int li = 0; li < (int)scene.entities.size(); ++li) {
            const Entity& le = scene.entities[li];
            if (le.kind == InstanceKind::Light && le.visible && le.cast_shadows &&
                le.light_type == LightType::Directional) {
                em::mat4 lw = scene.world_matrix(li);
                sd = em::normalize(em::vec3{-lw.m[1][0], -lw.m[1][1], -lw.m[1][2]});
                break;
            }
        }
        em::vec3 focus = {cam.position.x, 0.0f, cam.position.z};
        const float R = 34.0f, dist = 70.0f;
        em::vec3 eye = {focus.x - sd.x * dist, focus.y - sd.y * dist, focus.z - sd.z * dist};
        em::vec3 up = (std::fabs(sd.y) > 0.95f) ? em::vec3{0, 0, 1} : em::vec3{0, 1, 0};
        lightVP = em::ortho(-R, R, -R, R, 0.1f, dist * 2.0f) * em::look_at(eye, focus, up);

        SDL_GPUDepthStencilTargetInfo sdt{};
        sdt.texture = shadow_map_;
        sdt.clear_depth = 1.0f;
        sdt.load_op = SDL_GPU_LOADOP_CLEAR;
        sdt.store_op = SDL_GPU_STOREOP_STORE;      // keep it — we sample it next
        sdt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        sdt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* sp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &sdt);
        SDL_GPUViewport sv{0, 0, (float)shadow_dim_, (float)shadow_dim_, 0.0f, 1.0f};
        SDL_SetGPUViewport(sp, &sv);
        SDL_BindGPUGraphicsPipeline(sp, shadow_pipeline_);
        for (int ei = 0; ei < (int)scene.entities.size(); ++ei) {
            const Entity& e = scene.entities[ei];
            if (!e.visible || e.mesh < 0 || e.mesh >= (int)scene.meshes.size()) continue;
            const GpuMesh& mesh = scene.meshes[e.mesh];
            if (!mesh.vbo || !mesh.ibo || mesh.index_count == 0) continue;
            MeshVertexUBO vubo; vubo.model = scene.world_matrix(ei); vubo.mvp = lightVP * vubo.model;
            SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
            SDL_GPUBufferBinding vb{}; vb.buffer = mesh.vbo; SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
            SDL_GPUBufferBinding ib{}; ib.buffer = mesh.ibo; SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(sp, mesh.index_count, 1, 0, 0, 0);
        }
        SDL_EndGPURenderPass(sp);
    }

    // ---- Pass 1: 3D scene ----
    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = SDL_FColor{scene.sky_color.x, scene.sky_color.y, scene.sky_color.z, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;   // keep depth so overlays can occlude
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    // Confine the 3D scene to the editor viewport rect (the rest stays clear and
    // is covered by UI panels) so the preview sits in the middle, not behind.
    SDL_GPUViewport gvp{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &gvp);
    SDL_Rect gsc{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h};
    SDL_SetGPUScissor(pass, &gsc);
    SDL_BindGPUGraphicsPipeline(pass, mesh_pipeline_);

    // Build the punctual-light array once for this frame (shared by every draw).
    // A light emits along its local -Y axis (identity rotation -> points down).
    GpuLight frame_lights[kMaxLights];
    int light_count = 0;
    for (int li = 0; li < (int)scene.entities.size() && light_count < kMaxLights; ++li) {
        const Entity& le = scene.entities[li];
        if (le.kind != InstanceKind::Light || !le.visible) continue;
        em::mat4 lw = scene.world_matrix(li);
        em::vec3 lpos{lw.m[3][0], lw.m[3][1], lw.m[3][2]};
        em::vec3 ldir = em::normalize(em::vec3{-lw.m[1][0], -lw.m[1][1], -lw.m[1][2]});
        const float D2R = 3.14159265f / 180.0f;
        GpuLight& g = frame_lights[light_count++];
        g.position  = em::vec4(lpos, (float)(int)le.light_type);
        g.direction = em::vec4(ldir, le.light_range);
        g.color     = em::vec4(le.light_color, le.light_intensity);
        g.params    = em::vec4{std::cos(le.spot_inner_deg * D2R), std::cos(le.spot_outer_deg * D2R),
                               le.area_size.x, le.area_size.y};
    }

    for (int ei = 0; ei < (int)scene.entities.size(); ++ei) {
        const Entity& e = scene.entities[ei];
        if (!e.visible || e.mesh < 0 || e.mesh >= (int)scene.meshes.size()) continue;
        const GpuMesh& mesh = scene.meshes[e.mesh];
        if (!mesh.vbo || !mesh.ibo || mesh.index_count == 0) continue;
        const Material* mat = lookup(materials, e.material);

        MeshVertexUBO vubo;
        vubo.model = scene.world_matrix(ei);
        vubo.mvp   = proj * view * vubo.model;
        SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));

        MeshFragUBO fubo;
        fubo.baseColor = mat ? mat->color_tint : em::vec4{0.8f, 0.8f, 0.8f, 1.0f};
        fubo.lightDir  = em::vec4(scene.sun_dir, mat && mat->use_texture ? 1.0f : 0.0f);
        fubo.cameraPos = em::vec4(cam.position, 1.0f);
        fubo.params    = {mat ? mat->metalness : 0.0f, mat ? mat->roughness : 0.5f,
                          (mat && mat->use_mr) ? 1.0f : 0.0f,       // z = sample MR map
                          (mat && mat->use_normal) ? 1.0f : 0.0f};  // w = sample normal map
        // World lighting: colored sun + colored ambient fill. Material ambient
        // acts as a per-material ambient-occlusion multiplier on the world fill.
        float matAO   = mat ? (mat->ambient / 0.15f) : 1.0f;   // 0.15 == neutral default
        fubo.sunColor     = em::vec4(scene.sun_color, scene.sun_intensity);
        fubo.ambientColor = em::vec4(scene.ambient_color, scene.ambient_intensity * matAO);
        fubo.lightInfo    = em::vec4{(float)light_count, (float)scene.shading_mode, scene.exposure, (float)scene.shadow_samples};
        for (int k = 0; k < kMaxLights; ++k) fubo.lights[k] = frame_lights[k];
        fubo.lightVP      = lightVP;
        fubo.shadowParams = em::vec4{scene.shadow_strength, shadows_on ? 1.0f : 0.0f,
                                     1.0f / (float)(shadow_dim_ ? shadow_dim_ : 1),
                                     scene.shadow_soft ? scene.shadow_softness : 0.35f};
        SDL_PushGPUFragmentUniformData(cmd, 0, &fubo, sizeof(fubo));

        SDL_GPUBufferBinding vbind{}; vbind.buffer = mesh.vbo;
        SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);
        SDL_GPUBufferBinding ibind{}; ibind.buffer = mesh.ibo;
        SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_GPUTextureSamplerBinding tex[4]{};
        tex[0].texture = (mat && mat->tex) ? mat->tex : white_;             // albedo
        tex[0].sampler = sampler_;
        tex[1].texture = shadow_map_ ? shadow_map_ : white_;               // shadow
        tex[1].sampler = sampler_;
        tex[2].texture = (mat && mat->tex_mr) ? mat->tex_mr : white_;      // metal-rough
        tex[2].sampler = sampler_;
        tex[3].texture = (mat && mat->tex_normal) ? mat->tex_normal : white_; // normal
        tex[3].sampler = sampler_;
        SDL_BindGPUFragmentSamplers(pass, 0, tex, 4);

        SDL_DrawGPUIndexedPrimitives(pass, mesh.index_count, 1, 0, 0, 0);
    }

    SDL_EndGPURenderPass(pass);

    // ---- selection outline: silhouette mask + full-screen edge detect ----
    // Outlines every entity in `highlight` AND its descendants (group highlight).
    if (!highlight.empty() && mask_pipeline_ && outline_post_pipeline_) {
        auto is_hl = [&](int idx) {
            int g = 0;
            while (idx >= 0 && idx < (int)scene.entities.size() && g++ < 64) {
                for (int s : highlight) if (s == idx) return true;
                idx = scene.entities[idx].parent;
            }
            return false;
        };
        ensure_mask(w, h);
        SDL_GPUColorTargetInfo mc{};
        mc.texture = mask_; mc.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        mc.load_op = SDL_GPU_LOADOP_CLEAR; mc.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* mp = SDL_BeginGPURenderPass(cmd, &mc, 1, nullptr);
        SDL_GPUViewport mgvp{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f};
        SDL_SetGPUViewport(mp, &mgvp);
        SDL_BindGPUGraphicsPipeline(mp, mask_pipeline_);
        for (int i = 0; i < (int)scene.entities.size(); ++i) {
            const Entity& e = scene.entities[i];
            if (!e.visible || e.mesh < 0 || e.mesh >= (int)scene.meshes.size() || !is_hl(i)) continue;
            const GpuMesh& mesh = scene.meshes[e.mesh];
            if (!mesh.vbo || !mesh.ibo || !mesh.index_count) continue;
            MeshVertexUBO vubo; vubo.model = scene.world_matrix(i); vubo.mvp = proj * view * vubo.model;
            SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
            SDL_GPUBufferBinding vb{}; vb.buffer = mesh.vbo; SDL_BindGPUVertexBuffers(mp, 0, &vb, 1);
            SDL_GPUBufferBinding ib{}; ib.buffer = mesh.ibo; SDL_BindGPUIndexBuffer(mp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_DrawGPUIndexedPrimitives(mp, mesh.index_count, 1, 0, 0, 0);
        }
        SDL_EndGPURenderPass(mp);

        // (b) edge-detect the mask and composite the outline on screen.
        SDL_GPUColorTargetInfo oc{};
        oc.texture = target; oc.load_op = SDL_GPU_LOADOP_LOAD; oc.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* opass = SDL_BeginGPURenderPass(cmd, &oc, 1, nullptr);
        SDL_Rect osc{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h};
        SDL_SetGPUScissor(opass, &osc);
        SDL_BindGPUGraphicsPipeline(opass, outline_post_pipeline_);
        SDL_GPUTextureSamplerBinding mb{}; mb.texture = mask_; mb.sampler = sampler_;
        SDL_BindGPUFragmentSamplers(opass, 0, &mb, 1);
        struct { float color[4]; float params[4]; } oubo;
        oubo.color[0] = 1.0f; oubo.color[1] = 0.55f; oubo.color[2] = 0.10f; oubo.color[3] = 1.0f;
        oubo.params[0] = 1.0f / (float)w; oubo.params[1] = 1.0f / (float)h;
        oubo.params[2] = 3.0f; oubo.params[3] = 0.0f;
        SDL_PushGPUFragmentUniformData(cmd, 0, &oubo, sizeof(oubo));
        SDL_DrawGPUPrimitives(opass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(opass);
    }

    // ---- collider debug visualization (per-shape clean overlay) ----
    if (show_colliders && collider_fill_pipeline_ && collider_edge_pipeline_ && collider_line_pipeline_) {
        bool any = false;
        for (const Entity& ce : scene.entities)
            if (ce.collider != ColliderType::None) { any = true; break; }
        if (any) {
            // Pick X-ray (always-on-top, no depth) or depth-tested (occluded by geometry).
            SDL_GPUGraphicsPipeline* pipe_fill = colliders_xray ? collider_fill_pipeline_ : collider_fill_depth_;
            SDL_GPUGraphicsPipeline* pipe_line = colliders_xray ? collider_line_pipeline_ : collider_line_depth_;
            SDL_GPUGraphicsPipeline* pipe_edge = colliders_xray ? collider_edge_pipeline_ : collider_edge_depth_;
            SDL_GPUColorTargetInfo cc{};
            cc.texture = target; cc.load_op = SDL_GPU_LOADOP_LOAD; cc.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo cd{};
            cd.texture = depth_; cd.load_op = SDL_GPU_LOADOP_LOAD; cd.store_op = SDL_GPU_STOREOP_DONT_CARE;
            cd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; cd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass* cpass = SDL_BeginGPURenderPass(cmd, &cc, 1, colliders_xray ? nullptr : &cd);
            SDL_GPUViewport cvp{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f};
            SDL_SetGPUViewport(cpass, &cvp);
            SDL_Rect csc{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h};
            SDL_SetGPUScissor(cpass, &csc);
            auto draw = [&](SDL_GPUGraphicsPipeline* pipe, const GpuMesh& gm,
                            const em::mat4& model, float alpha) {
                if (!gm.vbo || !gm.ibo) return;
                SDL_BindGPUGraphicsPipeline(cpass, pipe);
                MeshVertexUBO vubo; vubo.model = model; vubo.mvp = proj * view * model;
                SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
                MeshFragUBO fubo{};
                fubo.baseColor = em::vec4{0.28f, 0.95f, 0.45f, alpha};
                fubo.lightDir  = em::vec4(scene.sun_dir, 0.0f);
                fubo.cameraPos = em::vec4(cam.position, 1.0f);
                fubo.params    = {0.0f, 0.5f, 1.0f, 0.0f};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fubo, sizeof(fubo));
                SDL_GPUBufferBinding vbb{}; vbb.buffer = gm.vbo;
                SDL_BindGPUVertexBuffers(cpass, 0, &vbb, 1);
                SDL_GPUBufferBinding ibb{}; ibb.buffer = gm.ibo;
                SDL_BindGPUIndexBuffer(cpass, &ibb, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUTextureSamplerBinding txb{}; txb.texture = white_; txb.sampler = sampler_;
                SDL_BindGPUFragmentSamplers(cpass, 0, &txb, 1);
                SDL_DrawGPUIndexedPrimitives(cpass, gm.index_count, 1, 0, 0, 0);
            };
            for (int ci = 0; ci < (int)scene.entities.size(); ++ci) {
                const Entity& ce = scene.entities[ci];
                if (ce.collider == ColliderType::None) continue;
                if (ce.mesh < 0 || ce.mesh >= (int)scene.meshes.size()) continue;
                const GpuMesh& mm = scene.meshes[ce.mesh];
                if (!mm.vbo || !mm.ibo) continue;
                em::vec3 cen{(mm.bounds_min.x + mm.bounds_max.x) * 0.5f,
                             (mm.bounds_min.y + mm.bounds_max.y) * 0.5f,
                             (mm.bounds_min.z + mm.bounds_max.z) * 0.5f};
                em::vec3 ext{mm.bounds_max.x - mm.bounds_min.x,
                             mm.bounds_max.y - mm.bounds_min.y,
                             mm.bounds_max.z - mm.bounds_min.z};
                const em::mat4 base = scene.world_matrix(ci);
                const em::quat noq{0, 0, 0, 1};
                if (ce.collider == ColliderType::Box) {
                    em::mat4 m = base * em::compose(cen, noq, ext);
                    draw(pipe_fill, collider_cube_, m, 0.12f);     // faint faces
                    draw(pipe_edge, collider_box_edges_, m, 0.9f); // clean 12 edges
                } else if (ce.collider == ColliderType::Sphere || ce.collider == ColliderType::Capsule) {
                    float r = ext.x * 0.5f;
                    if (ext.y * 0.5f > r) r = ext.y * 0.5f;
                    if (ext.z * 0.5f > r) r = ext.z * 0.5f;
                    // three great-circle rings fake a sphere without triangle soup
                    draw(pipe_edge, collider_ring_, base * g_model({r,0,0},{0,r,0},{0,0,r}, cen, 1.0f), 0.9f); // XY
                    draw(pipe_edge, collider_ring_, base * g_model({0,r,0},{0,0,r},{r,0,0}, cen, 1.0f), 0.9f); // YZ
                    draw(pipe_edge, collider_ring_, base * g_model({r,0,0},{0,0,r},{0,r,0}, cen, 1.0f), 0.9f); // XZ
                } else {   // Convex: show the actual mesh as a wireframe hull
                    draw(pipe_line, mm, base, 0.7f);
                }
            }
            SDL_EndGPURenderPass(cpass);
        }
    }

    // ---- camera frustum visuals (so Camera instances are visible) ----
    if (collider_edge_pipeline_ && gizmo_frustum_.vbo) {
        bool anyc = false;
        for (const Entity& e : scene.entities) if (e.kind == InstanceKind::Camera) { anyc = true; break; }
        if (anyc) {
            SDL_GPUColorTargetInfo fc{};
            fc.texture = target; fc.load_op = SDL_GPU_LOADOP_LOAD; fc.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* fp = SDL_BeginGPURenderPass(cmd, &fc, 1, nullptr);
            SDL_GPUViewport fv{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f}; SDL_SetGPUViewport(fp, &fv);
            SDL_Rect fsc{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h}; SDL_SetGPUScissor(fp, &fsc);
            SDL_BindGPUGraphicsPipeline(fp, collider_edge_pipeline_);
            for (int ci = 0; ci < (int)scene.entities.size(); ++ci) {
                if (scene.entities[ci].kind != InstanceKind::Camera) continue;
                em::mat4 model = scene.world_matrix(ci) * em::compose({0, 0, 0}, em::quat{0, 0, 0, 1}, {0.6f, 0.6f, 0.6f});
                MeshVertexUBO vubo; vubo.model = model; vubo.mvp = proj * view * model;
                SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
                MeshFragUBO fubo{};
                fubo.baseColor = {0.30f, 0.90f, 1.0f, 0.95f};
                fubo.lightDir = em::vec4(scene.sun_dir, 0.0f);
                fubo.cameraPos = em::vec4(cam.position, 1.0f);
                fubo.params = {0.0f, 0.5f, 1.0f, 0.0f};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fubo, sizeof(fubo));
                SDL_GPUBufferBinding vb{}; vb.buffer = gizmo_frustum_.vbo; SDL_BindGPUVertexBuffers(fp, 0, &vb, 1);
                SDL_GPUBufferBinding ib{}; ib.buffer = gizmo_frustum_.ibo; SDL_BindGPUIndexBuffer(fp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUTextureSamplerBinding tx{}; tx.texture = white_; tx.sampler = sampler_; SDL_BindGPUFragmentSamplers(fp, 0, &tx, 1);
                SDL_DrawGPUIndexedPrimitives(fp, gizmo_frustum_.index_count, 1, 0, 0, 0);
            }
            SDL_EndGPURenderPass(fp);
        }
    }

    // ---- light visuals (wireframe marker so Light instances are visible) ----
    if (collider_edge_pipeline_ && collider_ring_.vbo) {
        bool anyl = false;
        for (const Entity& e : scene.entities) if (e.kind == InstanceKind::Light && e.visible) { anyl = true; break; }
        if (anyl) {
            SDL_GPUColorTargetInfo lc{};
            lc.texture = target; lc.load_op = SDL_GPU_LOADOP_LOAD; lc.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* lp = SDL_BeginGPURenderPass(cmd, &lc, 1, nullptr);
            SDL_GPUViewport lv{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f}; SDL_SetGPUViewport(lp, &lv);
            SDL_Rect lsc{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h}; SDL_SetGPUScissor(lp, &lsc);
            SDL_BindGPUGraphicsPipeline(lp, collider_edge_pipeline_);
            // Draw one wireframe gizmo mesh with a given model matrix + color.
            auto draw_gizmo = [&](const GpuMesh& gm, const em::mat4& model, em::vec4 col) {
                if (!gm.vbo || !gm.ibo) return;
                MeshVertexUBO vubo; vubo.model = model; vubo.mvp = proj * view * model;
                SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
                MeshFragUBO fubo{};              // defaults -> Phong, no lights, exposure 1
                fubo.baseColor = col;
                fubo.lightDir = em::vec4(scene.sun_dir, 0.0f);
                fubo.cameraPos = em::vec4(cam.position, 1.0f);
                fubo.params = {0.0f, 0.5f, 1.0f, 0.0f};
                SDL_PushGPUFragmentUniformData(cmd, 0, &fubo, sizeof(fubo));
                SDL_GPUBufferBinding vb{}; vb.buffer = gm.vbo; SDL_BindGPUVertexBuffers(lp, 0, &vb, 1);
                SDL_GPUBufferBinding ib{}; ib.buffer = gm.ibo; SDL_BindGPUIndexBuffer(lp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUTextureSamplerBinding tx{}; tx.texture = white_; tx.sampler = sampler_; SDL_BindGPUFragmentSamplers(lp, 0, &tx, 1);
                SDL_DrawGPUIndexedPrimitives(lp, gm.index_count, 1, 0, 0, 0);
            };
            const em::quat rings[3] = { em::quat{0, 0, 0, 1},
                                        em::quat::from_euler(1.5708f, 0, 0),
                                        em::quat::from_euler(0, 1.5708f, 0) };
            const em::quat qid{0, 0, 0, 1};
            for (int li = 0; li < (int)scene.entities.size(); ++li) {
                const Entity& e = scene.entities[li];
                if (e.kind != InstanceKind::Light || !e.visible) continue;
                em::mat4 wm = scene.world_matrix(li);
                em::vec3 lpos{wm.m[3][0], wm.m[3][1], wm.m[3][2]};
                em::vec3 c = e.light_color;
                float mx = std::max(std::max(c.x, c.y), std::max(c.z, 0.35f));
                em::vec4 col{c.x / mx, c.y / mx, c.z / mx, 1.0f};   // normalize -> bright marker
                switch (e.light_type) {
                    case LightType::Point:
                        for (int r = 0; r < 3; ++r)
                            draw_gizmo(collider_ring_, em::compose(lpos, rings[r], {0.35f, 0.35f, 0.35f}), col);
                        break;
                    case LightType::Spot: {
                        float L = std::max(1.5f, std::min(e.light_range, 6.0f));
                        float rad = std::tan(e.spot_outer_deg * 3.14159265f / 180.0f) * L;
                        draw_gizmo(light_cone_, wm * em::compose({0, 0, 0}, qid, {rad, L, rad}), col);
                    } break;
                    case LightType::Directional:
                        draw_gizmo(light_dir_, wm * em::compose({0, 0, 0}, qid, {1.3f, 1.3f, 1.3f}), col);
                        break;
                    case LightType::Area:
                        draw_gizmo(light_quad_, wm * em::compose({0, 0, 0}, qid, {std::max(0.1f, e.area_size.x), 1.0f, std::max(0.1f, e.area_size.y)}), col);
                        break;
                }
            }
            SDL_EndGPURenderPass(lp);
        }
    }

    // ---- 3D transform gizmo (always-on-top handles, screen-constant size) ----
    if (selected >= 0 && selected < (int)scene.entities.size() && gizmo_pipeline_ &&
        gizmo_mode >= 0 && gizmo_mode <= 2) {
        const Entity& e = scene.entities[selected];
        const em::mat4 wm = scene.world_matrix(selected);
        const em::vec3 gpos = {wm.m[3][0], wm.m[3][1], wm.m[3][2]};
        em::vec3 dv{cam.position.x - gpos.x, cam.position.y - gpos.y, cam.position.z - gpos.z};
        float dist = std::sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
        float gs = dist * 0.13f; if (gs < 0.05f) gs = 0.05f;
        const em::mat4 br = em::quat_to_mat4(e.transform.rotation);
        const em::vec3 axcol[3] = {{1.0f, 0.22f, 0.28f}, {0.30f, 0.92f, 0.32f}, {0.24f, 0.50f, 1.0f}};
        const GpuMesh& gm = (gizmo_mode == 1) ? gizmo_ring_
                          : (gizmo_mode == 2) ? gizmo_scale_ : gizmo_arrow_;

        SDL_GPUColorTargetInfo gc{};
        gc.texture = target; gc.load_op = SDL_GPU_LOADOP_LOAD; gc.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo gd{};
        gd.texture = depth_; gd.clear_depth = 1.0f;
        gd.load_op = SDL_GPU_LOADOP_CLEAR; gd.store_op = SDL_GPU_STOREOP_DONT_CARE;
        gd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; gd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* gpass = SDL_BeginGPURenderPass(cmd, &gc, 1, &gd);
        SDL_GPUViewport gvp2{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f};
        SDL_SetGPUViewport(gpass, &gvp2);
        SDL_Rect gsc2{(int)vp_x, (int)vp_y, (int)vp_w, (int)vp_h};
        SDL_SetGPUScissor(gpass, &gsc2);
        SDL_BindGPUGraphicsPipeline(gpass, gizmo_pipeline_);
        if (gm.vbo && gm.ibo) {
            for (int k = 0; k < 3; ++k) {
                em::vec3 dir = gizmo_local
                    ? em::normalize(em::vec3{br.m[k][0], br.m[k][1], br.m[k][2]})
                    : em::vec3{k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f};
                em::mat4 model;
                if (gizmo_mode == 1) {   // ring: local +Z is the rotation axis
                    em::vec3 z = dir;
                    em::vec3 rf = (std::fabs(z.z) > 0.9f) ? em::vec3{1, 0, 0} : em::vec3{0, 0, 1};
                    em::vec3 x = em::normalize(em::cross(rf, z));
                    em::vec3 y = em::cross(z, x);
                    model = g_model(x, y, z, gpos, gs);
                } else {                 // arrow/scale: local +Y points down the axis
                    em::vec3 y = dir;
                    em::vec3 rf = (std::fabs(y.y) > 0.9f) ? em::vec3{1, 0, 0} : em::vec3{0, 1, 0};
                    em::vec3 x = em::normalize(em::cross(rf, y));
                    em::vec3 z = em::cross(y, x);
                    model = g_model(x, y, z, gpos, gs);
                }
                MeshVertexUBO vubo; vubo.model = model; vubo.mvp = proj * view * model;
                SDL_PushGPUVertexUniformData(cmd, 0, &vubo, sizeof(vubo));
                bool hot = (gizmo_hot == k);   // per-axis highlight for all modes
                em::vec3 col = hot ? em::vec3{1.0f, 0.92f, 0.20f} : axcol[k];
                MeshFragUBO fubo{};
                fubo.baseColor = em::vec4(col, 1.0f);
                fubo.lightDir  = em::vec4(scene.sun_dir, 0.0f);
                fubo.cameraPos = em::vec4(cam.position, 1.0f);
                fubo.params    = {0.0f, 0.5f, 0.55f, 0.0f};   // ambient .55 -> bright half-lambert
                SDL_PushGPUFragmentUniformData(cmd, 0, &fubo, sizeof(fubo));
                SDL_GPUBufferBinding vb2{}; vb2.buffer = gm.vbo;
                SDL_BindGPUVertexBuffers(gpass, 0, &vb2, 1);
                SDL_GPUBufferBinding ib2{}; ib2.buffer = gm.ibo;
                SDL_BindGPUIndexBuffer(gpass, &ib2, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUTextureSamplerBinding tx2{}; tx2.texture = white_; tx2.sampler = sampler_;
                SDL_BindGPUFragmentSamplers(gpass, 0, &tx2, 1);
                SDL_DrawGPUIndexedPrimitives(gpass, gm.index_count, 1, 0, 0, 0);
            }
        }
        SDL_EndGPURenderPass(gpass);
    }

    // ---- Pass 2: UI overlay (color only, no depth) ----
    SDL_GPUColorTargetInfo ui_color{};
    ui_color.texture = target;
    ui_color.load_op = SDL_GPU_LOADOP_LOAD;
    ui_color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* ui_pass = SDL_BeginGPURenderPass(cmd, &ui_color, 1, nullptr);
    ui.render(cmd, ui_pass, (int)w, (int)h);
    SDL_EndGPURenderPass(ui_pass);

    // ---- optional in-engine screenshot: blit the offscreen frame to the window,
    //      then download it + write a PNG (no external capture tool needed) ----
    if (capturing) {
        SDL_GPUBlitInfo bi{};
        bi.source.texture = capture_tex_; bi.source.w = w; bi.source.h = h;
        bi.destination.texture = swap; bi.destination.w = w; bi.destination.h = h;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &bi);

        const uint32_t bytes = w * h * 4;
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; tbi.size = bytes;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &tbi);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{}; src.texture = target; src.w = w; src.h = h; src.d = 1;
        SDL_GPUTextureTransferInfo dst{}; dst.transfer_buffer = tb; dst.offset = 0;
        dst.pixels_per_row = w; dst.rows_per_layer = h;
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);
        if (void* map = SDL_MapGPUTransferBuffer(dev_, tb, false)) {
            std::vector<uint8_t> px(bytes);
            std::memcpy(px.data(), map, bytes);
            SDL_UnmapGPUTransferBuffer(dev_, tb);
            // swapchain is B8G8R8A8 (format 12) -> swap R/B and force opaque alpha
            for (uint32_t i = 0; i < bytes; i += 4) { std::swap(px[i], px[i + 2]); px[i + 3] = 255; }
            if (stbi_write_png(screenshot_path_.c_str(), (int)w, (int)h, 4, px.data(), (int)w * 4))
                println("[screenshot] wrote %s (%ux%u)", screenshot_path_.c_str(), w, h);
            else
                warnln("[screenshot] failed to write %s", screenshot_path_.c_str());
        }
        SDL_ReleaseGPUTransferBuffer(dev_, tb);
        screenshot_path_.clear();
        return true;   // already submitted with a fence above
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    return true;
}

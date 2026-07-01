#include "pch.hpp"
#include "gltf/model.hpp"
#include "gpu/gpu.hpp"
#include "cgltf.h"   // declarations only; CGLTF_IMPLEMENTATION lives in thirdparty.cpp
#include "stb_image.h" // declarations only; implementation in thirdparty.cpp

#include <cfloat>

void MeshData::recompute_bounds() {
    if (vertices.empty()) { bounds_min = bounds_max = em::vec3(0); return; }
    em::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const auto& v : vertices) {
        mn.x = std::min(mn.x, v.pos.x); mn.y = std::min(mn.y, v.pos.y); mn.z = std::min(mn.z, v.pos.z);
        mx.x = std::max(mx.x, v.pos.x); mx.y = std::max(mx.y, v.pos.y); mx.z = std::max(mx.z, v.pos.z);
    }
    bounds_min = mn; bounds_max = mx;
}

namespace model {

// ----------------------------------------------------------------------------- glb
static em::vec3 transform_point(const float m[16], const em::vec3& p) {
    return {
        m[0]*p.x + m[4]*p.y + m[8]*p.z  + m[12],
        m[1]*p.x + m[5]*p.y + m[9]*p.z  + m[13],
        m[2]*p.x + m[6]*p.y + m[10]*p.z + m[14],
    };
}
static em::vec3 transform_normal(const float m[16], const em::vec3& n) {
    // upper 3x3 (ignores non-uniform scale correction — fine for our purposes)
    return em::normalize(em::vec3{
        m[0]*n.x + m[4]*n.y + m[8]*n.z,
        m[1]*n.x + m[5]*n.y + m[9]*n.z,
        m[2]*n.x + m[6]*n.y + m[10]*n.z,
    });
}

bool load_glb(const std::string& path, Model& out) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        warnln("cgltf: failed to parse %s", path.c_str());
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        warnln("cgltf: failed to load buffers for %s", path.c_str());
        cgltf_free(data);
        return false;
    }

    out.primitives.clear();
    out.source_path = path;

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        cgltf_node* node = &data->nodes[ni];
        if (!node->mesh) continue;

        float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi) {
            cgltf_primitive* prim = &node->mesh->primitives[pi];
            if (prim->type != cgltf_primitive_type_triangles) continue;

            Primitive outp;
            cgltf_accessor* pos = nullptr;
            cgltf_accessor* nrm = nullptr;
            cgltf_accessor* uv0 = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                cgltf_attribute* at = &prim->attributes[a];
                if (at->type == cgltf_attribute_type_position) pos = at->data;
                else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
                else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv0 = at->data;
            }
            if (!pos) continue;

            const cgltf_size vcount = pos->count;
            outp.mesh.vertices.resize(vcount);
            for (cgltf_size v = 0; v < vcount; ++v) {
                Vertex& vert = outp.mesh.vertices[v];
                float tmp[4] = {0, 0, 0, 0};
                cgltf_accessor_read_float(pos, v, tmp, 3);
                vert.pos = transform_point(world, {tmp[0], tmp[1], tmp[2]});
                if (nrm) {
                    cgltf_accessor_read_float(nrm, v, tmp, 3);
                    vert.normal = transform_normal(world, {tmp[0], tmp[1], tmp[2]});
                }
                if (uv0) {
                    cgltf_accessor_read_float(uv0, v, tmp, 2);
                    vert.uv = {tmp[0], tmp[1]};
                }
            }

            if (prim->indices) {
                const cgltf_size icount = prim->indices->count;
                outp.mesh.indices.resize(icount);
                for (cgltf_size i = 0; i < icount; ++i)
                    outp.mesh.indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i));
            } else {
                outp.mesh.indices.resize(vcount);
                for (cgltf_size i = 0; i < vcount; ++i) outp.mesh.indices[i] = static_cast<uint32_t>(i);
            }

            if (prim->material) {
                if (prim->material->name) outp.material_name = prim->material->name;
                // Decode an embedded texture (PNG/JPG in a buffer view) into RGBA.
                auto decode_tex = [&](const cgltf_texture_view& tv,
                                      std::vector<uint8_t>& dst, int& dw, int& dh) {
                    if (!(tv.texture && tv.texture->image && tv.texture->image->buffer_view)) return;
                    cgltf_buffer_view* bv = tv.texture->image->buffer_view;
                    if (!(bv->buffer && bv->buffer->data)) return;
                    const unsigned char* src = (const unsigned char*)bv->buffer->data + bv->offset;
                    int tw = 0, th = 0, tn = 0;
                    unsigned char* px = stbi_load_from_memory(src, (int)bv->size, &tw, &th, &tn, 4);
                    if (px) { dst.assign(px, px + (size_t)tw * th * 4); dw = tw; dh = th; stbi_image_free(px); }
                };
                if (prim->material->has_pbr_metallic_roughness) {
                    const cgltf_pbr_metallic_roughness& pmr = prim->material->pbr_metallic_roughness;
                    const float* c = pmr.base_color_factor;
                    outp.base_color = {c[0], c[1], c[2], c[3]};
                    outp.metalness  = pmr.metallic_factor;
                    outp.roughness  = pmr.roughness_factor;
                    decode_tex(pmr.base_color_texture, outp.tex_rgba, outp.tex_w, outp.tex_h);
                    if (!outp.tex_rgba.empty() && pmr.base_color_texture.texture->image)
                        outp.tex_key = path + "#" + std::to_string((size_t)(pmr.base_color_texture.texture->image - data->images));
                    decode_tex(pmr.metallic_roughness_texture, outp.mr_rgba, outp.mr_w, outp.mr_h);
                }
                decode_tex(prim->material->normal_texture, outp.nrm_rgba, outp.nrm_w, outp.nrm_h);
            }
            outp.mesh.recompute_bounds();
            out.primitives.push_back(std::move(outp));
        }
    }

    cgltf_free(data);
    println("Loaded GLB '%s': %zu primitive(s)", path.c_str(), out.primitives.size());
    return !out.primitives.empty();
}

// ----------------------------------------------------------------------------- procedural
MeshData make_cube(float h) {
    MeshData m;
    struct Face { em::vec3 n, u, v; };
    const Face faces[6] = {
        {{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, // +Z
        {{ 0, 0,-1}, {-1,0, 0}, {0, 1, 0}}, // -Z
        {{ 1, 0, 0}, {0, 0,-1}, {0, 1, 0}}, // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, // -X
        {{ 0, 1, 0}, {1, 0, 0}, {0, 0,-1}}, // +Y
        {{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}}, // -Y
    };
    for (const auto& f : faces) {
        em::vec3 c = f.n * h;
        em::vec3 corners[4] = {
            c + f.u * (-h) + f.v * (-h),
            c + f.u * ( h) + f.v * (-h),
            c + f.u * ( h) + f.v * ( h),
            c + f.u * (-h) + f.v * ( h),
        };
        em::vec2 uvs[4] = {{0,1},{1,1},{1,0},{0,0}};
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (int i = 0; i < 4; ++i) m.vertices.push_back({corners[i], f.n, uvs[i]});
        m.indices.insert(m.indices.end(), {base, base+1, base+2, base, base+2, base+3});
    }
    m.recompute_bounds();
    return m;
}

MeshData make_plane(float h, float tiles) {
    MeshData m;
    em::vec3 n{0, 1, 0};
    m.vertices = {
        {{-h, 0, -h}, n, {0, 0}},
        {{ h, 0, -h}, n, {tiles, 0}},
        {{ h, 0,  h}, n, {tiles, tiles}},
        {{-h, 0,  h}, n, {0, tiles}},
    };
    m.indices = {0, 2, 1, 0, 3, 2};
    m.recompute_bounds();
    return m;
}

MeshData make_sphere(float radius, int segments) {
    MeshData m;
    for (int y = 0; y <= segments; ++y) {
        float v = float(y) / segments;
        float phi = v * em::PI;
        for (int x = 0; x <= segments; ++x) {
            float u = float(x) / segments;
            float theta = u * em::TAU;
            em::vec3 nrm{ std::sin(phi)*std::cos(theta), std::cos(phi), std::sin(phi)*std::sin(theta) };
            m.vertices.push_back({ nrm * radius, nrm, {u, v} });
        }
    }
    int stride = segments + 1;
    for (int y = 0; y < segments; ++y)
        for (int x = 0; x < segments; ++x) {
            uint32_t a = y*stride + x, b = a + 1, c = a + stride, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    m.recompute_bounds();
    return m;
}

// ----------------------------------------------------------------------------- gpu upload
GpuMesh upload(SDL_GPUDevice* dev, const MeshData& data) {
    GpuMesh g;
    g.index_count = static_cast<uint32_t>(data.indices.size());
    g.bounds_min = data.bounds_min;
    g.bounds_max = data.bounds_max;
    g.vbo = gpu::upload_buffer(dev, data.vertices.data(),
                               static_cast<uint32_t>(data.vertices.size() * sizeof(Vertex)),
                               SDL_GPU_BUFFERUSAGE_VERTEX, "mesh.vbo");
    g.ibo = gpu::upload_buffer(dev, data.indices.data(),
                               static_cast<uint32_t>(data.indices.size() * sizeof(uint32_t)),
                               SDL_GPU_BUFFERUSAGE_INDEX, "mesh.ibo");
    return g;
}

void destroy(SDL_GPUDevice* dev, GpuMesh& mesh) {
    if (mesh.vbo) SDL_ReleaseGPUBuffer(dev, mesh.vbo);
    if (mesh.ibo) SDL_ReleaseGPUBuffer(dev, mesh.ibo);
    mesh = {};
}

} // namespace model

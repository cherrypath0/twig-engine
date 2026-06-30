#include "pch.hpp"
#include "gpu/gpu.hpp"
#include <fstream>
#include <vector>

namespace gpu {

SDL_GPUBuffer* upload_buffer(SDL_GPUDevice* dev, const void* data,
                             uint32_t size, uint32_t usage_flags,
                             const char* debug_name) {
    if (size == 0) return nullptr;

    SDL_GPUBufferCreateInfo bci{};
    bci.usage = usage_flags;
    bci.size  = size;
    SDL_GPUBuffer* buf = SDL_CreateGPUBuffer(dev, &bci);
    if (!buf) { warnln("SDL_CreateGPUBuffer(%s) failed: %s", debug_name, SDL_GetError()); return nullptr; }
    if (debug_name) SDL_SetGPUBufferName(dev, buf, debug_name);

    SDL_GPUTransferBufferCreateInfo tci{};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size  = size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tci);

    void* map = SDL_MapGPUTransferBuffer(dev, tb, false);
    std::memcpy(map, data, size);
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = tb; src.offset = 0;
    SDL_GPUBufferRegion dst{}; dst.buffer = buf; dst.offset = 0; dst.size = size;
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return buf;
}

SDL_GPUTexture* create_texture_rgba(SDL_GPUDevice* dev, const void* pixels,
                                    uint32_t w, uint32_t h, const char* debug_name) {
    SDL_GPUTextureCreateInfo tci{};
    tci.type   = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage  = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width  = w;
    tci.height = h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &tci);
    if (!tex) { warnln("SDL_CreateGPUTexture(%s) failed: %s", debug_name, SDL_GetError()); return nullptr; }
    if (debug_name) SDL_SetGPUTextureName(dev, tex, debug_name);

    const uint32_t bytes = w * h * 4;
    SDL_GPUTransferBufferCreateInfo bci{};
    bci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bci.size  = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &bci);
    void* map = SDL_MapGPUTransferBuffer(dev, tb, false);
    std::memcpy(map, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb; src.offset = 0;
    src.pixels_per_row = w; src.rows_per_layer = h;
    SDL_GPUTextureRegion dst{};
    dst.texture = tex; dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return tex;
}

SDL_GPUTexture* create_white_texture(SDL_GPUDevice* dev) {
    const uint32_t white = 0xFFFFFFFFu;
    return create_texture_rgba(dev, &white, 1, 1, "white");
}

SDL_GPUSampler* create_linear_sampler(SDL_GPUDevice* dev) {
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    return SDL_CreateGPUSampler(dev, &sci);
}

SDL_GPUShader* load_spirv(SDL_GPUDevice* dev, const std::string& path, int stage,
                          uint32_t num_samplers, uint32_t num_uniform_buffers) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { warnln("Shader not found: %s", path.c_str()); return nullptr; }
    std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<char> bytes(static_cast<size_t>(sz));
    f.read(bytes.data(), sz);

    SDL_GPUShaderCreateInfo sci{};
    sci.code      = reinterpret_cast<const Uint8*>(bytes.data());
    sci.code_size = bytes.size();
    sci.entrypoint = "main";
    sci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    sci.stage  = (stage == 0) ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
    sci.num_samplers         = num_samplers;
    sci.num_uniform_buffers  = num_uniform_buffers;
    sci.num_storage_buffers  = 0;
    sci.num_storage_textures = 0;

    SDL_GPUShader* sh = SDL_CreateGPUShader(dev, &sci);
    if (!sh) warnln("SDL_CreateGPUShader(%s) failed: %s", path.c_str(), SDL_GetError());
    return sh;
}

} // namespace gpu

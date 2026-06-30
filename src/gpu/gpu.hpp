#pragma once
// Thin helpers over the SDL3 GPU API: buffer / texture uploads and SPIR-V
// shader loading. Everything here is backend-agnostic engine plumbing.
#include <cstdint>
#include <string>

struct SDL_GPUDevice;
struct SDL_GPUBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUShader;

namespace gpu {

// Upload raw bytes into a new GPU buffer with the given usage flags
// (SDL_GPU_BUFFERUSAGE_VERTEX / _INDEX). Blocking upload via a copy pass.
SDL_GPUBuffer* upload_buffer(SDL_GPUDevice* dev, const void* data,
                             uint32_t size, uint32_t usage_flags,
                             const char* debug_name);

// Create an RGBA8 sampled texture from tightly-packed pixel data.
SDL_GPUTexture* create_texture_rgba(SDL_GPUDevice* dev, const void* pixels,
                                    uint32_t w, uint32_t h, const char* debug_name);

// A 1x1 white texture, handy as the "no texture" default.
SDL_GPUTexture* create_white_texture(SDL_GPUDevice* dev);

SDL_GPUSampler* create_linear_sampler(SDL_GPUDevice* dev);

// Load a compiled SPIR-V file and create a shader object. `stage` is 0 = vertex,
// 1 = fragment (mirrors SDL_GPUShaderStage to avoid leaking SDL into headers).
SDL_GPUShader* load_spirv(SDL_GPUDevice* dev, const std::string& path, int stage,
                          uint32_t num_samplers, uint32_t num_uniform_buffers);

} // namespace gpu

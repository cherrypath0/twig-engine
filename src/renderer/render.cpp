#include "pch.hpp"
#include "render.hpp"

bool render_frame(SDL_GPUDevice* gpu, SDL_Window* window) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) {
        warnln("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTexture* swapchain_texture = nullptr;
    Uint32 tex_w = 0, tex_h = 0;

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window,
                                               &swapchain_texture,
                                               &tex_w, &tex_h)) {
        warnln("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }

    if (!swapchain_texture) {
        SDL_CancelGPUCommandBuffer(cmd);
        return true;
    }

    SDL_GPUColorTargetInfo color_info{};
    color_info.texture = swapchain_texture;
    color_info.clear_color = SDL_FColor{0.1f, 0.2f, 0.4f, 1.0f};
    color_info.load_op = SDL_GPU_LOADOP_CLEAR;
    color_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_info, 1, nullptr);

    // ----------------------------------------------------
    // Your world rendering will go here later:
    //   - bind pipelines
    //   - draw meshes / sprites
    //   - etc.
    // ----------------------------------------------------

    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(cmd);

    return true;
}
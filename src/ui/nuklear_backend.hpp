#pragma once
// Renders Nuklear's immediate-mode UI through the SDL3 GPU API.
//
// Frame flow (driven by the renderer):
//     input_begin();
//     for each SDL_Event:  handle_event(e);
//     input_end();
//     ... editor builds the UI with ctx() ...
//     prepare(cmd);                         // BEFORE any render pass (copy pass)
//     ... 3D scene render pass ...
//     render(cmd, pass, w, h);              // inside a color-only render pass
#include <cstdint>

struct SDL_GPUDevice;
struct SDL_Window;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
union  SDL_Event;
struct nk_context;
struct nk_user_font;

class NuklearBackend {
public:
    bool init(SDL_GPUDevice* dev, SDL_Window* window, uint32_t swapchain_format);
    void shutdown();

    nk_context* ctx();

    // Baked fonts: Fira Sans (UI, also the default) and JetBrains Mono (code).
    struct nk_user_font* ui_font();
    struct nk_user_font* mono_font();

    void input_begin();
    void handle_event(const SDL_Event& e);
    void input_end();

    // Convert + upload this frame's geometry (must be outside a render pass).
    void prepare(SDL_GPUCommandBuffer* cmd);
    // Draw the prepared geometry (must be inside a color render pass).
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, int fb_w, int fb_h);

    struct Impl;   // defined in nuklear_backend.cpp
private:
    Impl* p = nullptr;
};

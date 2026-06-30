// PRE-COMPILED HEADERS
#include "pch.hpp"

// SOURCE FILES
#include "renderer/render.hpp"

int main() {
    devwarnln("Current channel is DEV, extended debugging is enabled");
    println("Initializing engine");

    try {
        std::filesystem::create_directory("logs");
    } catch (const std::exception& ex) {
        warnln("Failed to create logs directory: %s", ex.what());
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        warnln("SDL_Init failed: %s", SDL_GetError());
        return exitp(1);
    }

    println("Video driver: %s", SDL_GetCurrentVideoDriver());

    SDL_Window* window = SDL_CreateWindow(
        "Game Engine",
        1280, 720,
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        warnln("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return exitp(1);
    }

    Uint32 shader_formats = SDL_GPU_SHADERFORMAT_SPIRV;
    bool debug_mode = isDev;

    SDL_GPUDevice* gpu = SDL_CreateGPUDevice(
        static_cast<SDL_GPUShaderFormat>(shader_formats),
        debug_mode,
        "vulkan"
    );
    if (!gpu) {
        warnln("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return exitp(1);
    }

    const char* driver_name = SDL_GetGPUDeviceDriver(gpu);
    if (driver_name) {
        println("Renderer: %s", driver_name);
    } else {
        warnln("SDL_GetGPUDeviceDriver failed or returned NULL.");
    }

    SDL_PropertiesID props = SDL_GetGPUDeviceProperties(gpu);
    if (props != 0) {
        const char* dev_name = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
        const char* drv_name = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr);
        const char* drv_ver  = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, nullptr);

        if (!dev_name) dev_name = "unknown";
        if (!drv_name) drv_name = "unknown";
        if (!drv_ver)  drv_ver  = "unknown";

        println("GPU device: %s", dev_name);
        println("GPU driver version: %s %s", drv_name, drv_ver);
    } else {
        warnln("SDL_GetGPUDeviceProperties failed: %s", SDL_GetError());
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        warnln("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        SDL_DestroyGPUDevice(gpu);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return exitp(1);
    }

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        if (!render_frame(gpu, window)) {
            running = false;
        }
    }

    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return exitp(0);
}
#pragma once

struct SDL_GPUDevice;
struct SDL_Window;

bool render_frame(SDL_GPUDevice* gpu, SDL_Window* window);
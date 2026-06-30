// Single translation unit that compiles the implementations of all the
// header-only third-party libraries. Keeping them together stops the heavy
// implementation code from being recompiled across the rest of the engine.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define NK_IMPLEMENTATION
#include "ui/nuklear_config.hpp"

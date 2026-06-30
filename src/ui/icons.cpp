#include "pch.hpp"
#include "ui/icons.hpp"
#include "ui/nuklear_config.hpp"
#include "gpu/gpu.hpp"

// nanosvg is a single-header library: exactly one translation unit must define
// the IMPLEMENTATION macros before including the headers. This is that TU.
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

// Build an nk_image from a GPU texture handle. Nuklear carries the texture as
// an opaque pointer in dcmd->texture.ptr; our renderer reads it back per draw
// command (see nuklear_backend.cpp render()).
static struct nk_image image_from_tex(SDL_GPUTexture* tex) {
    return nk_image_handle(nk_handle_ptr(tex));
}

// Rasterize one SVG into a fresh RGBA SDL_Surface (e.g. for the window icon).
SDL_Surface* rasterize_svg_surface(const std::string& path, int px) {
    if (px <= 0) px = 256;
    NSVGimage* svg = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!svg) { warnln("rasterize_svg_surface: parse failed: %s", path.c_str()); return nullptr; }
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return nullptr; }
    SDL_Surface* surf = SDL_CreateSurface(px, px, SDL_PIXELFORMAT_RGBA32);
    if (!surf) { nsvgDeleteRasterizer(rast); nsvgDelete(svg); return nullptr; }
    float scale = 1.0f;
    if (svg->width > 0.0f && svg->height > 0.0f) {
        float sx = static_cast<float>(px) / svg->width;
        float sy = static_cast<float>(px) / svg->height;
        scale = sx < sy ? sx : sy;
    }
    SDL_memset(surf->pixels, 0, static_cast<size_t>(surf->pitch) * static_cast<size_t>(px));
    nsvgRasterize(rast, svg, 0.0f, 0.0f, scale,
                  static_cast<unsigned char*>(surf->pixels), px, px, surf->pitch);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
    return surf;
}

bool IconSet::init(SDL_GPUDevice* dev, const std::string& dir, int px) {
    dev_ = dev;
    px_  = px > 0 ? px : 36;

    // Transparent 1x1. Icons are currently rendered BLANK via get() (the SVG
    // textures sample the font atlas instead of their own -> garbled noise), so
    // a transparent fallback keeps the UI clean until the binding is fixed.
    const uint32_t transparent = 0u;
    fallback_tex_ = gpu::create_texture_rgba(dev_, &transparent, 1, 1, "blank");
    if (!fallback_tex_) {
        warnln("IconSet: failed to create fallback texture");
        return false;
    }
    fallback_ = image_from_tex(fallback_tex_);

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        warnln("IconSet: icon directory not found: %s", dir.c_str());
        return true;  // still usable via the fallback image
    }

    // One rasterizer reused for every icon.
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        warnln("IconSet: nsvgCreateRasterizer failed");
        return true;
    }

    const int w = px_, h = px_;
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);

    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& path = entry.path();
        if (path.extension() != ".svg") continue;

        // nsvgParseFromFile mutates its buffer, so we hand it a fresh copy of
        // the file each time (it reads the file itself here).
        NSVGimage* svg = nsvgParseFromFile(path.string().c_str(), "px", 96.0f);
        if (!svg) {
            warnln("IconSet: failed to parse %s", path.string().c_str());
            continue;
        }

        // Uniform scale that fits the SVG's native size into our square target.
        // Most icons use a 24x24 viewBox; using max() preserves aspect and
        // centers nothing-clipping for square art.
        float scale = 1.0f;
        if (svg->width > 0.0f && svg->height > 0.0f) {
            float sx = static_cast<float>(w) / svg->width;
            float sy = static_cast<float>(h) / svg->height;
            scale = sx < sy ? sx : sy;
        }

        std::fill(rgba.begin(), rgba.end(), static_cast<unsigned char>(0));
        nsvgRasterize(rast, svg, 0.0f, 0.0f, scale,
                      rgba.data(), w, h, w * 4);
        nsvgDelete(svg);

        const std::string stem = path.stem().string();
        SDL_GPUTexture* tex = gpu::create_texture_rgba(
            dev_, rgba.data(), static_cast<uint32_t>(w),
            static_cast<uint32_t>(h), stem.c_str());
        if (!tex) {
            warnln("IconSet: texture upload failed for %s", stem.c_str());
            continue;
        }

        textures_.push_back(tex);
        images_[stem] = image_from_tex(tex);
        ++loaded;
    }

    nsvgDeleteRasterizer(rast);
    println("IconSet: loaded %d icons from %s (%dx%d)", loaded, dir.c_str(), w, h);
    return true;
}

struct nk_image IconSet::get(const std::string& name) const {
    auto it = images_.find(name);
    return it != images_.end() ? it->second : fallback_;
}

bool IconSet::has(const std::string& name) const {
    return images_.find(name) != images_.end();
}

void IconSet::shutdown() {
    if (!dev_) return;
    for (SDL_GPUTexture* tex : textures_) {
        if (tex) SDL_ReleaseGPUTexture(dev_, tex);
    }
    textures_.clear();
    images_.clear();
    if (fallback_tex_) {
        SDL_ReleaseGPUTexture(dev_, fallback_tex_);
        fallback_tex_ = nullptr;
    }
    struct nk_image empty_img{};
    fallback_ = empty_img;
    dev_ = nullptr;
}

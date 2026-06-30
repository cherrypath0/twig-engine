#pragma once
// SVG icon system for the editor UI.
//
// IconSet loads every *.svg in a directory, rasterizes each to an RGBA8
// bitmap with nanosvg, uploads it as a GPU texture, and wraps it in an
// nk_image so the editor can draw it with nk_image / nk_button_image.
//
// Ownership: the Engine owns the GPU device, so the Engine should own the
// IconSet too. Call init() once after the GPU device + Nuklear backend exist,
// then pass a (const) reference/pointer into Editor::draw so panels can do:
//     ctx_button_image(iconset.get("mesh"), ...);
//
// IMPORTANT: this header pulls in Nuklear via nuklear_config.hpp so it has the
// real nk_image type. Only include it from translation units that already use
// the shared Nuklear configuration.
#include "ui/nuklear_config.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_Surface;

// Rasterize one SVG file into a fresh RGBA SDL_Surface (caller SDL_DestroySurface).
// Used for the window / taskbar icon. Returns nullptr on failure.
SDL_Surface* rasterize_svg_surface(const std::string& path, int px = 256);

// A set of rasterized SVG icons keyed by file stem (e.g. "mesh", "play").
class IconSet {
public:
    // Load and rasterize every *.svg in `dir` at `px` x `px` pixels. Returns
    // true if the set initialized (even if some individual icons failed). A
    // 1x1 white fallback texture is always created so get() never returns an
    // image with a null handle. Safe to call once; call shutdown() to release.
    bool init(SDL_GPUDevice* dev, const std::string& dir = "assets/icons", int px = 36);

    // Look up an icon by stem. Returns the fallback (1x1 white) image when the
    // name is unknown, so callers can use the result unconditionally.
    struct nk_image get(const std::string& name) const;

    // True if an icon with this stem was loaded successfully.
    bool has(const std::string& name) const;

    // Release every GPU texture (including the fallback) and clear the set.
    void shutdown();

    // Pixel size each icon was rasterized at (square). Useful for layout.
    int pixel_size() const { return px_; }

private:
    SDL_GPUDevice*                            dev_ = nullptr;
    int                                       px_ = 0;
    std::unordered_map<std::string, struct nk_image> images_;   // stem -> image
    std::vector<SDL_GPUTexture*>                     textures_;  // owned, for shutdown
    SDL_GPUTexture*                                  fallback_tex_ = nullptr;
    struct nk_image                                  fallback_{};
};

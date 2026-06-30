#pragma once
// A docking system for the Nuklear editor.
//
// Instead of free-floating windows, the editor content area is partitioned into
// fixed dock ZONES:
//
//     +--------------------------------------------------+
//     |                    top strip                     |  (menubar/toolbar)
//     +------+------------------------------------+------+
//     |      |                                    |      |
//     | Left |              Center                | Right|
//     |      |          (3D viewport)             |      |
//     |      |                                    |      |
//     +------+------------------------------------+------+
//     |                   Bottom                         |
//     +--------------------------------------------------+
//
// The borders between Left|Center|Right and between (the columns) and Bottom are
// draggable SPLITTERS, so the user can resize zones with the mouse. The split
// positions are stored as fractions and persist across frames.
//
// Every zone hosts one or more PANELS shown as TABS. Only the active tab's body
// is drawn; clicking a tab activates it. Any panel can be toggled fullscreen,
// in which case it covers the whole content area (below the top strip).
//
// The Center zone is intentionally left empty (no panel chrome) so the 3D
// viewport shows through and mouse picking still works there.
//
// Usage (immediate mode, every frame):
//
//     dock.begin(ctx, screen_w, screen_h);          // compute rects, draw tabs
//     if (dock.begin_panel(ctx, "Outliner", DockZone::Right)) {
//         ... fill the panel with normal nk_* widgets ...
//     }
//     dock.end_panel(ctx);
//     ... more panels ...
//     dock.end(ctx);
//
// IMPORTANT: include this for nk_* types via the project's nuklear_config.hpp;
// here we only forward-declare so the header stays light.

struct nk_context;
struct nk_image;

// Which zone a panel lives in.
enum class DockZone { Left, Right, Bottom, Center, Floating };

class DockSpace {
public:
    DockSpace();

    // Height of the reserved top strip (toolbar/menubar). The integrator should
    // set this to match the toolbar window height. Default 84 (matches the
    // editor's current toolbar).
    void set_top_strip(float h) { top_strip_ = h; }

    // Outer margin (in px) kept around the whole dock area, and the gap between
    // zones. Optional tuning knobs.
    void set_margin(float m) { margin_ = m; }
    void set_gap(float g)    { gap_ = g; }

    // Begin a docking frame: recomputes zone rectangles from the split
    // fractions, processes splitter dragging, and is ready to accept panels.
    // Must be called once per frame before any begin_panel().
    void begin(nk_context* ctx, int screen_w, int screen_h);

    // Register a panel into a zone and start its window if it is the active tab.
    // Returns true when THIS panel is the active tab of its zone, meaning the
    // caller should emit the panel body (nk_layout_* / widgets). When it returns
    // false the body must be skipped, but end_panel() must STILL be called.
    // `icon` (optional) is drawn on the tab button.
    // NOTE: `struct nk_image` (elaborated) is required because Nuklear declares
    // BOTH a struct nk_image AND a function nk_image(); the function name would
    // otherwise hide the type in TUs that include nuklear before this header.
    bool begin_panel(nk_context* ctx, const char* title, DockZone zone,
                     struct nk_image* icon = nullptr);

    // Close the most recent begin_panel(). Always pair with begin_panel(),
    // regardless of its return value.
    void end_panel(nk_context* ctx);

    // Finish the docking frame: draws splitter handles on top. Call after all
    // panels for this frame.
    void end(nk_context* ctx);

    // Toggle a panel's fullscreen state by its title. When on, that panel covers
    // the entire content area until toggled off. Only one panel can be
    // fullscreen at a time.
    void set_fullscreen(const char* title, bool on);
    bool is_fullscreen(const char* title) const;

    // The Center (3D viewport) rect in screen px, computed in begin(). The
    // renderer confines the scene to this so it sits cleanly in the middle.
    void center_rect(float& x, float& y, float& w, float& h) const;

private:
    struct Impl;
    // File-local helpers in dock.cpp need access to the private Impl state.
    friend void dock_draw_tabbar(nk_context*, Impl*, int);
    // Fixed-size inline storage so the header needs no <memory>/<vector>.
    // (Impl is defined in dock.cpp; we keep a pointer with manual lifetime.)
    Impl* d = nullptr;

    // Tunables (kept here so they survive without touching Impl every access).
    float top_strip_ = 84.0f;
    float margin_    = 6.0f;
    float gap_       = 4.0f;
};

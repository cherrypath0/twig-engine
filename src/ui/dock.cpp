#include "pch.hpp"
#include "ui/dock.hpp"
#include "ui/nuklear_config.hpp"

#include <string>
#include <vector>

// =============================================================================
// Internal state
// =============================================================================
//
// We model four dockable zones (Left, Right, Bottom, Center). Center is the
// viewport and never hosts panels. Each dockable zone keeps an ordered list of
// the panels that registered into it THIS frame plus a persistent "active tab"
// index keyed by panel title (so the selection survives even as the per-frame
// panel list is rebuilt).
//
// Layout is driven by three split fractions:
//   split_left_   : fraction of content width given to the Left column.
//   split_right_  : fraction of content width given to the Right column
//                   (measured from the right edge).
//   split_bottom_ : fraction of content height given to the Bottom row.
// The Center column gets whatever horizontal space remains; the top region
// (Left|Center|Right) gets whatever vertical space remains above Bottom.

namespace {

constexpr float kTabH      = 24.0f;  // tab-bar height
constexpr float kSplitterW = 6.0f;   // grab thickness of a splitter
constexpr float kMinFrac   = 0.08f;  // clamp so a zone never fully collapses
constexpr float kMaxFrac   = 0.60f;

// A persistent record of which tab is active within a zone. We store it by
// title string because the per-frame panel ordering may change.
struct ZoneState {
    std::string active_title;       // currently selected tab in this zone
};

// One panel as registered during the current frame.
struct PanelEntry {
    std::string title;
    struct nk_image* icon = nullptr;
};

inline struct nk_rect rect(float x, float y, float w, float h) { return ::nk_rect(x, y, w, h); }

} // namespace

struct DockSpace::Impl {
    // Persisted split fractions (survive across frames).
    float split_left   = 0.18f;
    float split_right  = 0.22f;
    float split_bottom = 0.26f;

    // Per-frame: panels registered into each zone, in registration order.
    std::vector<PanelEntry> panels[5];   // indexed by DockZone

    // Persistent active-tab selection for each zone.
    ZoneState zone[5];

    // Fullscreen panel (empty => none).
    std::string fullscreen_title;

    // Computed this frame in begin().
    int   screen_w = 0, screen_h = 0;
    float content_x = 0, content_y = 0, content_w = 0, content_h = 0;

    // Zone body rects (the area below the tab bar) computed in begin().
    struct nk_rect body[5]{};
    // Tab-bar rects.
    struct nk_rect tabbar[5]{};
    // Whether a zone is active (has any panels) this frame.
    bool has_panels[5]{};

    // Splitter hit rects (for drawing the handle in end()).
    struct nk_rect split_v_left{};   // between Left and Center
    struct nk_rect split_v_right{};  // between Center and Right
    struct nk_rect split_h_bottom{}; // between columns and Bottom
    struct nk_rect viewport{};       // Center zone (3D preview) rect

    // The panel currently open between begin_panel()/end_panel().
    bool  panel_open   = false;  // we called nk_begin and it returned true
    bool  panel_active = false;  // this panel is the active tab (caller fills it)
    DockZone panel_zone = DockZone::Center;
};

// =============================================================================
DockSpace::DockSpace() { d = new Impl(); }

void DockSpace::center_rect(float& x, float& y, float& w, float& h) const {
    x = d->viewport.x; y = d->viewport.y; w = d->viewport.w; h = d->viewport.h;
}

// Note: DockSpace is a long-lived editor member; its Impl is owned for the whole
// program lifetime. We deliberately do not declare a destructor in the header
// (Impl is incomplete there); the single allocation lives until process exit.

// -----------------------------------------------------------------------------
static int zone_index(DockZone z) { return static_cast<int>(z); }

// Drag a vertical splitter: updates `frac` from horizontal mouse motion while
// the left button is held over `handle`. `from_right` flips the sign so the
// Right column grows when dragged left. Returns true while dragging.
static bool drag_vertical(nk_context* ctx, struct nk_rect handle, float content_w,
                          float& frac, bool from_right) {
    const nk_input* in = &ctx->input;
    // Begin a drag when the user presses inside the handle...
    bool down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT);
    bool started_here =
        nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, handle, nk_true);
    if (down && started_here && content_w > 1.0f) {
        float dx = in->mouse.delta.x / content_w;
        frac += from_right ? -dx : dx;
        if (frac < kMinFrac) frac = kMinFrac;
        if (frac > kMaxFrac) frac = kMaxFrac;
        return true;
    }
    return false;
}

// Drag a horizontal splitter (Bottom row). Dragging up grows the Bottom row.
static bool drag_horizontal(nk_context* ctx, struct nk_rect handle, float content_h,
                            float& frac) {
    const nk_input* in = &ctx->input;
    bool down = nk_input_is_mouse_down(in, NK_BUTTON_LEFT);
    bool started_here =
        nk_input_has_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, handle, nk_true);
    if (down && started_here && content_h > 1.0f) {
        float dy = in->mouse.delta.y / content_h;
        frac -= dy;                       // up == grow
        if (frac < kMinFrac) frac = kMinFrac;
        if (frac > kMaxFrac) frac = kMaxFrac;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
void DockSpace::begin(nk_context* ctx, int screen_w, int screen_h) {
    Impl* s = d;
    s->screen_w = screen_w;
    s->screen_h = screen_h;

    // Clear the per-frame panel lists; selections in s->zone[] persist.
    for (int i = 0; i < 5; ++i) { s->panels[i].clear(); s->has_panels[i] = false; }

    // Content area = whole screen minus the top strip and outer margin.
    const float m  = margin_;
    s->content_x = m;
    s->content_y = top_strip_ + m;
    s->content_w = (float)screen_w - 2.0f * m;
    s->content_h = (float)screen_h - top_strip_ - 2.0f * m;
    if (s->content_w < 1.0f) s->content_w = 1.0f;
    if (s->content_h < 1.0f) s->content_h = 1.0f;

    const float gap = gap_;

    // --- horizontal split (columns) -----------------------------------------
    // First split off the Bottom row, the remaining "top" holds the columns.
    float bottom_h = s->content_h * s->split_bottom;
    float top_h    = s->content_h - bottom_h - gap;
    if (top_h < kTabH + 2.0f) top_h = kTabH + 2.0f;

    float left_w   = s->content_w * s->split_left;
    float right_w  = s->content_w * s->split_right;
    float center_w = s->content_w - left_w - right_w - 2.0f * gap;
    if (center_w < 40.0f) center_w = 40.0f;

    const float cx = s->content_x;
    const float cy = s->content_y;

    // Column x positions.
    float left_x   = cx;
    float center_x = cx + left_w + gap;
    float right_x  = center_x + center_w + gap;

    // Whole-rect (tab bar + body) for each zone.
    auto make_zone = [&](int idx, float x, float y, float w, float h) {
        s->tabbar[idx] = rect(x, y, w, kTabH);
        s->body[idx]   = rect(x, y + kTabH, w, h - kTabH);
    };
    make_zone(zone_index(DockZone::Left),   left_x,   cy, left_w,  top_h);
    make_zone(zone_index(DockZone::Center), center_x, cy, center_w, top_h);
    make_zone(zone_index(DockZone::Right),  right_x,  cy, right_w, top_h);
    s->viewport = rect(center_x, cy, center_w, top_h);

    // Bottom row spans the full content width, below the columns.
    float bottom_y = cy + top_h + gap;
    make_zone(zone_index(DockZone::Bottom), cx, bottom_y, s->content_w, bottom_h);

    // --- splitter hit rectangles (centered on the gaps) ---------------------
    s->split_v_left  = rect(center_x - gap - kSplitterW * 0.5f, cy,
                            kSplitterW, top_h);
    s->split_v_right = rect(right_x - gap - kSplitterW * 0.5f, cy,
                            kSplitterW, top_h);
    s->split_h_bottom = rect(cx, bottom_y - gap - kSplitterW * 0.5f,
                             s->content_w, kSplitterW);

    // --- process dragging ----------------------------------------------------
    drag_vertical(ctx, s->split_v_left,  s->content_w, s->split_left,  false);
    drag_vertical(ctx, s->split_v_right, s->content_w, s->split_right, true);
    drag_horizontal(ctx, s->split_h_bottom, s->content_h, s->split_bottom);
}

// -----------------------------------------------------------------------------
bool DockSpace::begin_panel(nk_context* ctx, const char* title, DockZone zone,
                            struct nk_image* icon) {
    Impl* s = d;
    s->panel_open   = false;
    s->panel_active = false;
    s->panel_zone   = zone;

    // Center never hosts panels (it's the viewport).
    if (zone == DockZone::Center) return false;

    const int zi = zone_index(zone);
    s->panels[zi].push_back(PanelEntry{ title, icon });
    s->has_panels[zi] = true;

    // First panel registered into an empty zone becomes the default active tab.
    if (s->zone[zi].active_title.empty())
        s->zone[zi].active_title = title;

    const bool fullscreen = (s->fullscreen_title == title);
    const bool is_active  = fullscreen || (s->zone[zi].active_title == title);
    if (!is_active) return false;   // not the visible tab -> caller skips body

    // Compute the window rect: fullscreen covers the whole content area, else
    // the zone's body rect.
    struct nk_rect r;
    if (fullscreen) {
        r = rect(s->content_x, s->content_y, s->content_w, s->content_h);
    } else {
        r = s->body[zi];
    }

    // Pin the window to its computed rect every frame (zones are not movable;
    // the splitters move them). nk_window_set_bounds enforces this even if
    // Nuklear had a stale stored position.
    // Use begin_titled so the *identifier* is stable while the visible title is
    // irrelevant (we draw our own tab bar, so no title bar flag).
    std::string id = std::string("dock_") + title;
    if (nk_begin_titled(ctx, id.c_str(), title, r,
                        NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_window_set_bounds(ctx, id.c_str(), r);
        s->panel_open   = true;
        s->panel_active = true;
        return true;
    }
    // nk_begin returned false (e.g. window minimized/closed): still need end().
    s->panel_open   = false;
    s->panel_active = true;   // it IS the active tab; body just isn't shown
    return false;
}

// -----------------------------------------------------------------------------
void DockSpace::end_panel(nk_context* ctx) {
    Impl* s = d;
    // We always pair nk_begin* with nk_end, regardless of visibility, as long as
    // we actually called nk_begin for this panel (active tab, non-center zone).
    if (s->panel_active)
        nk_end(ctx);
    s->panel_open   = false;
    s->panel_active = false;
}

// -----------------------------------------------------------------------------
// Draw the tab bar for one zone and handle tab clicks / fullscreen toggle.
void dock_draw_tabbar(nk_context* ctx, DockSpace::Impl* s, int zi) {
    if (!s->has_panels[zi]) return;
    const auto& panels = s->panels[zi];
    if (panels.empty()) return;

    struct nk_rect bar = s->tabbar[zi];

    // We draw the tab bar inside a borderless overlay window pinned to the tab
    // strip. It must remain interactive (tabs are buttons), so NO NK_WINDOW_NO_INPUT
    // here — only the splitter overlays use that flag.
    std::string id = std::string("dock_tabs_") + std::to_string(zi);
    if (nk_begin(ctx, id.c_str(), bar, NK_WINDOW_NO_SCROLLBAR)) {
        nk_window_set_bounds(ctx, id.c_str(), bar);
        struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
        if (canvas)
            nk_fill_rect(canvas, bar, 0.0f, nk_rgb(34, 36, 42));

        // Lay tabs out left-to-right. Reserve a small "fullscreen" button on the
        // far right.
        const int n = (int)panels.size();
        // tab buttons + one fullscreen toggle button
        nk_layout_row_template_begin(ctx, kTabH - 4.0f);
        for (int i = 0; i < n; ++i) nk_layout_row_template_push_static(ctx, 96);
        nk_layout_row_template_push_dynamic(ctx);          // spacer
        nk_layout_row_template_push_static(ctx, 28);       // [ ] fullscreen
        nk_layout_row_template_end(ctx);

        for (int i = 0; i < n; ++i) {
            const PanelEntry& pe = panels[i];
            const bool active = (s->zone[zi].active_title == pe.title);

            // Style the active tab differently from inactive ones.
            nk_style_button saved = ctx->style.button;
            if (active) {
                ctx->style.button.normal = nk_style_item_color(nk_rgb(60, 64, 74));
                ctx->style.button.hover  = nk_style_item_color(nk_rgb(70, 74, 86));
                ctx->style.button.active = nk_style_item_color(nk_rgb(60, 64, 74));
                ctx->style.button.text_normal = nk_rgb(255, 255, 255);
            } else {
                ctx->style.button.normal = nk_style_item_color(nk_rgb(40, 42, 50));
                ctx->style.button.hover  = nk_style_item_color(nk_rgb(52, 55, 64));
                ctx->style.button.active = nk_style_item_color(nk_rgb(52, 55, 64));
                ctx->style.button.text_normal = nk_rgb(180, 184, 192);
            }

            int clicked;
            if (pe.icon)
                clicked = nk_button_image_label(ctx, *pe.icon, pe.title.c_str(),
                                                NK_TEXT_LEFT);
            else
                clicked = nk_button_label(ctx, pe.title.c_str());
            ctx->style.button = saved;

            if (clicked) s->zone[zi].active_title = pe.title;
        }

        // spacer cell (consumed by the dynamic template column).
        nk_label(ctx, "", NK_TEXT_LEFT);

        // Fullscreen toggle for the active panel of this zone.
        const std::string& act = s->zone[zi].active_title;
        const bool is_fs = (s->fullscreen_title == act);
        if (nk_button_symbol(ctx, is_fs ? NK_SYMBOL_MINUS : NK_SYMBOL_PLUS)) {
            if (is_fs) s->fullscreen_title.clear();
            else       s->fullscreen_title = act;
        }
    }
    nk_end(ctx);
}

void DockSpace::end(nk_context* ctx) {
    Impl* s = d;

    // When a panel is fullscreen, only that zone's tab bar is meaningful; but we
    // still draw all zone tab bars so the user can switch/close. The fullscreen
    // body window (created in begin_panel) already covers the others.
    for (int zi = 0; zi < 5; ++zi) {
        if (zi == zone_index(DockZone::Center)) continue;
        dock_draw_tabbar(ctx, s, zi);
    }

    // Draw splitter handles as thin highlighted bars on top so they are visible
    // and hint at draggability. We use a tiny overlay window per splitter.
    auto draw_splitter = [&](const char* name, struct nk_rect r, bool vertical) {
        const nk_input* in = &ctx->input;
        bool hover = nk_input_is_mouse_hovering_rect(in, r);
        nk_color col = hover ? nk_rgb(90, 130, 200) : nk_rgb(55, 58, 66);
        if (nk_begin(ctx, name, r, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
            nk_window_set_bounds(ctx, name, r);
            struct nk_command_buffer* cv = nk_window_get_canvas(ctx);
            if (cv) {
                // Draw a centered hairline within the (thicker) hit rect.
                if (vertical) {
                    float x = r.x + r.w * 0.5f;
                    nk_stroke_line(cv, x, r.y + 2, x, r.y + r.h - 2, 2.0f, col);
                } else {
                    float y = r.y + r.h * 0.5f;
                    nk_stroke_line(cv, r.x + 2, y, r.x + r.w - 2, y, 2.0f, col);
                }
            }
        }
        nk_end(ctx);
    };
    draw_splitter("dock_split_l", s->split_v_left,  true);
    draw_splitter("dock_split_r", s->split_v_right, true);
    draw_splitter("dock_split_b", s->split_h_bottom, false);
}

// -----------------------------------------------------------------------------
void DockSpace::set_fullscreen(const char* title, bool on) {
    if (on) d->fullscreen_title = title ? title : "";
    else if (d->fullscreen_title == (title ? title : "")) d->fullscreen_title.clear();
}

bool DockSpace::is_fullscreen(const char* title) const {
    return title && d->fullscreen_title == title;
}

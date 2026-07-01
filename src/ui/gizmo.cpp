#include "pch.hpp"
// =============================================================================
//  gizmo.cpp  —  implementation of the transform manipulation gizmos.
// =============================================================================
#include "ui/gizmo.hpp"
#include "ui/nuklear_config.hpp"   // never <nuklear.h> directly
#include "core/picking.hpp"        // pick::screen_ray / inverse (reused helpers)

namespace {

// ----- tuning constants ------------------------------------------------------
constexpr float kAxisPixelLen   = 90.0f;   // on-screen length of each axis line
constexpr float kHandlePickDist = 18.0f;   // px: how close the mouse must be
constexpr float kLineThickness  = 2.5f;
constexpr float kRingRadiusPx   = 70.0f;   // rotate ring radius on screen
constexpr float kTranslateGain  = 1.0f;    // world units per "axis-pixel" delta
constexpr float kScaleGain      = 0.01f;   // scale change per pixel of drag
constexpr float kRotateGain     = 0.01f;   // radians per pixel of drag

// A projected point: screen pixel position + whether it is in front of camera.
struct Projected {
    em::vec2 screen;
    bool     visible;   // false if behind the camera / clipped at w<=0
};

// Project a world point to screen pixels using the same view*proj the renderer
// uses. SDL3-GPU clip space is 0..1 depth; we map NDC (-1..1) to pixels with
// the top-left origin convention used by SDL mouse coords (y grows downward).
Projected project(const em::mat4& vp, const em::vec3& world,
                  float vx, float vy, float screen_w, float screen_h) {
    em::vec4 clip = vp * em::vec4{world, 1.0f};
    Projected p;
    if (clip.w <= 1e-6f) {     // at or behind the camera plane
        p.screen  = {0, 0};
        p.visible = false;
        return p;
    }
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    // NDC +Y is up; screen +Y is down -> flip.
    p.screen.x = vx + (ndc_x * 0.5f + 0.5f) * screen_w;
    p.screen.y = vy + (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_h;
    p.visible  = true;
    return p;
}

// Shortest distance from point p to the segment [a,b], all in screen pixels.
float dist_point_segment(em::vec2 p, em::vec2 a, em::vec2 b) {
    em::vec2 ab {b.x - a.x, b.y - a.y};
    em::vec2 ap {p.x - a.x, p.y - a.y};
    float len2 = ab.x * ab.x + ab.y * ab.y;
    float t = (len2 > 1e-6f) ? (ap.x * ab.x + ap.y * ab.y) / len2 : 0.0f;
    t = em::clampf(t, 0.0f, 1.0f);
    em::vec2 proj {a.x + ab.x * t, a.y + ab.y * t};
    float dx = p.x - proj.x, dy = p.y - proj.y;
    return std::sqrt(dx * dx + dy * dy);
}

// The three world-space axis directions of a transform (its rotated basis).
// Index 0=X, 1=Y, 2=Z.
em::vec3 axis_world(const Transform& xform, int i, bool local) {
    if (!local)   // global: fixed world X/Y/Z axes
        return em::vec3{ i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f };
    em::mat4 r = em::quat_to_mat4(xform.rotation);
    // Column i of the rotation matrix is the rotated local axis.
    return em::normalize(em::vec3{r.m[i][0], r.m[i][1], r.m[i][2]});
}

// Broso/Hammer palette (matches the user's brosoforgejo editor_gizmo.gd).
const struct nk_color kAxisColor[3] = {
    {255,  56,  71, 255},   // X red   (1.00, 0.22, 0.28)
    { 77, 235,  82, 255},   // Y green (0.30, 0.92, 0.32)
    { 61, 128, 255, 255},   // Z blue  (0.24, 0.50, 1.00)
};
const struct nk_color kAxisHot[3] = {
    {255, 199, 140, 255},   // X hot (1.00, 0.78, 0.55)
    {209, 255, 158, 255},   // Y hot (0.82, 1.00, 0.62)
    {166, 217, 255, 255},   // Z hot (0.65, 0.85, 1.00)
};

} // namespace

GizmoResult Gizmo::manipulate(nk_context* ctx, Transform& xform, const Camera& cam,
                              float vx, float vy, float vw, float vh,
                              bool mouse_down, float mx, float my) {
    GizmoResult result;

    const float sw = vw;
    const float sh = vh;
    const float aspect = (sh > 0.0f) ? sw / sh : 1.0f;

    const em::mat4 vp = cam.proj(aspect) * cam.view();
    const em::vec3 origin = xform.position;

    // ----- project the origin and the three axis endpoints -------------------
    Projected o = project(vp, origin, vx, vy, sw, sh);

    // We size the world-space axis length so that the projected line is roughly
    // kAxisPixelLen pixels regardless of distance. Estimate scale from a small
    // probe: project origin + camera-right by a unit and measure pixels/unit.
    // Match the 3D gizmo mesh exactly: the renderer scales handles by
    // distance*0.13 world units, so the hit-test must use the same length.
    em::vec3 cdv{cam.position.x - origin.x, cam.position.y - origin.y, cam.position.z - origin.z};
    float gdist = std::sqrt(cdv.x * cdv.x + cdv.y * cdv.y + cdv.z * cdv.z);
    float world_len = gdist * 0.13f;
    if (world_len < 0.05f) world_len = 0.05f;

    em::vec3 axis_dir[3] = {
        axis_world(xform, 0, local_space), axis_world(xform, 1, local_space), axis_world(xform, 2, local_space)
    };
    Projected end[3];
    for (int i = 0; i < 3; ++i)
        end[i] = project(vp, origin + axis_dir[i] * world_len, vx, vy, sw, sh);

    // ----- determine which handle (if any) the mouse is near -----------------
    int hot_axis = -1;
    if (o.visible) {
        if (mode == GizmoMode::Rotate) {
            // Pick the closest of the three axis rings (X/Y/Z) by sampling each
            // ring in world space and measuring the screen distance to the mouse.
            float best = kHandlePickDist * 1.8f;
            for (int i = 0; i < 3; ++i) {
                em::vec3 n = axis_dir[i];
                em::vec3 ref = (std::fabs(n.y) > 0.9f) ? em::vec3{1, 0, 0} : em::vec3{0, 1, 0};
                em::vec3 u = em::normalize(em::cross(ref, n));
                em::vec3 v = em::cross(n, u);
                const int SEG = 40;
                Projected prev{}; bool have_prev = false;
                for (int s = 0; s <= SEG; ++s) {
                    float a = (float)s / SEG * 6.2831853f;
                    em::vec3 p = origin + (u * std::cos(a) + v * std::sin(a)) * (0.9f * world_len);
                    Projected pp = project(vp, p, vx, vy, sw, sh);
                    if (have_prev && pp.visible && prev.visible) {
                        float dd = dist_point_segment({mx, my}, prev.screen, pp.screen);
                        if (dd < best) { best = dd; hot_axis = i; }
                    }
                    prev = pp; have_prev = true;
                }
            }
        } else {
            float best = kHandlePickDist;
            for (int i = 0; i < 3; ++i) {
                if (!end[i].visible) continue;
                float d = dist_point_segment({mx, my}, o.screen, end[i].screen);
                if (d < best) { best = d; hot_axis = i; }
            }
        }
    }

    // ----- interaction state machine -----------------------------------------
    bool just_pressed = mouse_down && !prev_down_;

    if (dragging_) {
        if (!mouse_down) {
            // Drag released.
            dragging_ = false;
            axis_     = -1;
        } else {
            float dmx = mx - last_mx_;
            float dmy = my - last_my_;
            const bool ctrl = nk_input_is_key_down(&ctx->input, NK_KEY_CTRL);

            if (mode == GizmoMode::Translate && axis_ >= 0) {
                // Move along the world axis proportional to the mouse delta
                // projected onto the axis's *screen* direction.
                em::vec2 ascr {end[axis_].screen.x - o.screen.x,
                               end[axis_].screen.y - o.screen.y};
                float alen2 = ascr.x * ascr.x + ascr.y * ascr.y;
                if (alen2 > 1e-6f) {
                    float along = (dmx * ascr.x + dmy * ascr.y) / std::sqrt(alen2);
                    // along is in screen pixels; convert to world units using
                    // the same px<->world scale used to size the axis.
                    float world_per_px = world_len / kAxisPixelLen;
                    xform.position += axis_dir[axis_] *
                                      (along * world_per_px * kTranslateGain);
                }
                if (snap_move > 0.0f) {
                    float step = ctrl ? snap_move * 0.125f : snap_move;
                    float* pc = (axis_ == 0) ? &xform.position.x : (axis_ == 1) ? &xform.position.y : &xform.position.z;
                    *pc = std::round(*pc / step) * step;
                }
            } else if (mode == GizmoMode::Scale && axis_ >= 0) {
                // Drag right/up grows, left/down shrinks. Project onto screen
                // axis direction for an intuitive feel.
                em::vec2 ascr {end[axis_].screen.x - o.screen.x,
                               end[axis_].screen.y - o.screen.y};
                float alen2 = ascr.x * ascr.x + ascr.y * ascr.y;
                float along = (alen2 > 1e-6f)
                    ? (dmx * ascr.x + dmy * ascr.y) / std::sqrt(alen2)
                    : dmx;
                float factor = 1.0f + along * kScaleGain;
                if (factor < 0.01f) factor = 0.01f;
                float* s = (axis_ == 0) ? &xform.scale.x
                         : (axis_ == 1) ? &xform.scale.y : &xform.scale.z;
                *s *= factor;
                if (snap_move > 0.0f) {
                    float step = ctrl ? snap_move * 0.125f : snap_move;
                    *s = std::round(*s / step) * step;
                    if (*s < 0.001f) *s = step;
                }
            } else if (mode == GizmoMode::Rotate && axis_ >= 0) {
                // Rotate about the SELECTED axis (X/Y/Z) by the change in the
                // mouse's angle around the gizmo centre — a proper per-axis ring.
                float a0 = std::atan2(last_my_ - o.screen.y, last_mx_ - o.screen.x);
                float a1 = std::atan2(my - o.screen.y, mx - o.screen.x);
                float dAng = a1 - a0;
                if (dAng >  3.14159265f) dAng -= 6.2831853f;
                if (dAng < -3.14159265f) dAng += 6.2831853f;
                // Flip when the axis faces away from the camera so it tracks the ring.
                float sgn = (em::dot(axis_dir[axis_], em::normalize(cam.forward())) > 0.0f) ? -1.0f : 1.0f;
                float angle = dAng * sgn;
                if (snap_rot_deg > 0.0f) {   // quantise the drag into fixed steps
                    float step = (ctrl ? snap_rot_deg * 0.125f : snap_rot_deg) * 0.0174532925f;
                    rot_accum_ += angle; angle = 0.0f;
                    while (rot_accum_ >=  step) { angle += step; rot_accum_ -= step; }
                    while (rot_accum_ <= -step) { angle -= step; rot_accum_ += step; }
                }
                if (angle != 0.0f) {
                    em::quat dq = em::quat::from_axis_angle(axis_dir[axis_], angle);
                    const em::quat& q = xform.rotation;
                    xform.rotation = em::quat{
                        dq.w*q.x + dq.x*q.w + dq.y*q.z - dq.z*q.y,
                        dq.w*q.y - dq.x*q.z + dq.y*q.w + dq.z*q.x,
                        dq.w*q.z + dq.x*q.y - dq.y*q.x + dq.z*q.w,
                        dq.w*q.w - dq.x*q.x - dq.y*q.y - dq.z*q.z
                    };
                    em::quat& nq = xform.rotation;
                    float nlen = std::sqrt(nq.x*nq.x + nq.y*nq.y + nq.z*nq.z + nq.w*nq.w);
                    if (nlen > 1e-8f) { nq.x/=nlen; nq.y/=nlen; nq.z/=nlen; nq.w/=nlen; }
                }
            }
            result.active = true;
        }
    } else if (just_pressed) {
        // Begin a drag if the press landed on a handle (rotate is now per-axis too).
        bool started = false;
        if (hot_axis >= 0) {
            dragging_ = true; axis_ = hot_axis; result.active = true; started = true; rot_accum_ = 0.0f;
        }
        // Shift + drag-start duplicates the selection (the drag then moves the clone).
        if (started && nk_input_is_key_down(&ctx->input, NK_KEY_SHIFT)) result.clone = true;
    }

    result.hovered = (hot_axis >= 0) || dragging_;
    result.axis    = dragging_ ? axis_ : hot_axis;

    // ----- draw the overlay --------------------------------------------------
    // A fullscreen, input-less, chromeless window whose canvas we stroke onto.
    // NK_WINDOW_NO_INPUT guarantees we never steal clicks from editor panels.
    // CRITICAL: hide the window background — a normal fullscreen window paints a
    // solid fill that would cover the entire 3D viewport (the scene would vanish
    // the moment an entity is selected).
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_hide());
    if (nk_begin(ctx, "##gizmo_overlay", nk_rect(vx, vy, sw, sh),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT |
                 NK_WINDOW_BACKGROUND)) {
        struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
        if (canvas && o.visible && draw_overlay) {
            if (mode == GizmoMode::Rotate) {
                struct nk_color col = (hot_axis >= 0) || dragging_
                    ? nk_rgb(255, 230, 120) : nk_rgb(220, 200, 90);
                struct nk_rect rr = nk_rect(o.screen.x - kRingRadiusPx,
                                            o.screen.y - kRingRadiusPx,
                                            kRingRadiusPx * 2.0f,
                                            kRingRadiusPx * 2.0f);
                nk_stroke_circle(canvas, rr, kLineThickness, col);
            } else {
                for (int i = 0; i < 3; ++i) {
                    if (!end[i].visible) continue;
                    bool hot = (i == hot_axis) || (dragging_ && i == axis_);
                    struct nk_color col = hot ? kAxisHot[i] : kAxisColor[i];
                    nk_stroke_line(canvas,
                                   o.screen.x, o.screen.y,
                                   end[i].screen.x, end[i].screen.y,
                                   kLineThickness, col);
                    // A small filled tip so the handle is easy to grab.
                    struct nk_rect tip = nk_rect(end[i].screen.x - 4.0f,
                                                 end[i].screen.y - 4.0f, 8.0f, 8.0f);
                    nk_fill_rect(canvas, tip, 1.0f, col);
                }
            }
        }
    }
    nk_end(ctx);
    nk_style_pop_style_item(ctx);

    // ----- bookkeeping -------------------------------------------------------
    last_mx_   = mx;
    last_my_   = my;
    prev_down_ = mouse_down;
    return result;
}

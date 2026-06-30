#pragma once
// =============================================================================
//  gizmo.hpp  —  transform manipulation gizmos (move / rotate / scale).
//
//  The gizmo is drawn as a 2D overlay on the Nuklear canvas: the selected
//  entity's three local axes are projected from world space to screen pixels
//  and rendered as colored lines (X=red, Y=green, Z=blue). The user drags an
//  axis handle to translate or scale along that axis, or drags anywhere to
//  rotate about the camera's view direction.
//
//  Hit-testing is done manually (mouse-to-segment distance) rather than through
//  Nuklear widgets, so the overlay window can be NK_WINDOW_NO_INPUT and never
//  steal clicks from the editor panels.
//
//  Usage (per frame, for the selected entity, BEFORE feeding camera input):
//      GizmoResult g = gizmo.manipulate(ctx, entity.transform, cam,
//                                       screen_w, screen_h,
//                                       mouse_left_down, mouse_x, mouse_y);
//      if (g.active) { /* suppress camera / right-mouse look this frame */ }
// =============================================================================
#include "math.hpp"
#include "scene/scene.hpp"   // Transform
#include "camera/camera.hpp"

struct nk_context;

enum class GizmoMode { Translate, Rotate, Scale };

// Returned each frame. `active` is true while the user is dragging a handle —
// the engine should ignore camera/right-mouse input that frame.
struct GizmoResult {
    bool hovered = false;  // mouse is over a handle (not necessarily dragging)
    bool active  = false;  // a drag is in progress -> suppress camera
    int  axis    = -1;     // hovered/active handle: 0=X 1=Y 2=Z, 3=rotate-ring, -1=none
    bool clone   = false;  // a drag began with Shift held -> duplicate first
};

class Gizmo {
public:
    GizmoMode mode = GizmoMode::Translate;
    bool local_space = false;   // false = global (world) axes, true = object-local
    bool draw_overlay = true;   // false = suppress 2D overlay (3D mesh handles visuals)
    float snap_move = 0.0f;     // 0 = off; world units to snap translate/scale to
    float snap_rot_deg = 0.0f;  // 0 = off; degrees to snap rotation to
private:
    float rot_accum_ = 0.0f;
public:

    // Draw and interact with the gizmo for one selected transform.
    //   ctx          : Nuklear context (overlay window built internally).
    //   xform        : the transform being edited (mutated in place).
    //   cam          : active camera (for projection).
    //   screen_w/h   : framebuffer size in pixels.
    //   mouse_down   : left mouse button state THIS frame.
    //   mx, my       : mouse position in pixels (SDL top-left origin).
    GizmoResult manipulate(nk_context* ctx, Transform& xform, const Camera& cam,
                           float vx, float vy, float vw, float vh,
                           bool mouse_down, float mx, float my);

private:
    // Drag state, persisted between frames while a handle is held.
    bool  dragging_   = false;   // a drag is currently in progress
    int   axis_       = -1;      // 0=X 1=Y 2=Z (-1 = none / view axis for rotate)
    float last_mx_    = 0.0f;    // previous-frame mouse position
    float last_my_    = 0.0f;
    bool  prev_down_  = false;   // previous-frame mouse button (edge detect)
};

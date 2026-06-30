#pragma once
// =============================================================================
//  picking.hpp  —  header-only mouse ray construction + ray/primitive tests.
//
//  Used by the editor to convert a screen-space mouse click into a world-space
//  ray, then test that ray against entity bounds (AABB) or bounding spheres to
//  determine which entity the user clicked on.
//
//  Conventions (must match math.hpp):
//    * Right-handed world space.
//    * em::perspective() produces Vulkan/SDL3-GPU clip space: depth 0..1, and
//      clip.y is flipped relative to GL (NDC +Y is down on screen in SDL GPU's
//      default, but because the renderer also flips, we treat NDC +Y as "up").
//      We unproject by inverting view*proj, so the exact axis convention is
//      handled consistently as long as the *same* matrices the renderer uses
//      are fed in here.
//    * Screen origin is top-left (mouse coordinates from SDL), +X right,
//      +Y down — the standard SDL mouse convention.
// =============================================================================
#include "math.hpp"
#include "camera/camera.hpp"

namespace pick {

// A world-space ray. `dir` is normalized.
struct Ray {
    em::vec3 origin {0, 0, 0};
    em::vec3 dir    {0, 0, -1};
};

// ----------------------------------------------------------------------------
//  4x4 inverse (general, column-major) — small helper kept local to picking so
//  math.hpp stays minimal. Returns identity on a singular matrix.
// ----------------------------------------------------------------------------
inline em::mat4 inverse(const em::mat4& m) {
    // Flatten column-major storage (m.m[col][row]) into a 16-float array laid
    // out as a[col*4 + row] so indexing matches the classic cofactor formula.
    const float* a = &m.m[0][0];
    float inv[16];

    inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] +
               a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] -
               a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] +
               a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] -
               a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] -
               a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] +
               a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] -
               a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] +
               a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] +
               a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] -
               a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] +
               a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] -
               a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] -
               a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] +
               a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] -
               a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] +
               a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];

    float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
    if (det > -1e-12f && det < 1e-12f) return em::mat4::identity();
    float idet = 1.0f / det;

    em::mat4 r;
    float* o = &r.m[0][0];
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * idet;
    return r;
}

// ----------------------------------------------------------------------------
//  screen_ray — unproject a mouse pixel into a world-space ray.
//
//  mouse_x/mouse_y are SDL mouse coords (top-left origin, +Y down).
//  screen_w/screen_h are the framebuffer pixel dimensions.
//  aspect should be screen_w/screen_h (passed explicitly so the caller can use
//  whatever the renderer uses).
// ----------------------------------------------------------------------------
inline Ray screen_ray(const Camera& cam, float aspect,
                      float mouse_x, float mouse_y,
                      float screen_w, float screen_h) {
    // Pixel -> NDC. NDC x in [-1,1] left..right.  For y we map top-left pixel
    // origin to NDC +Y up (top), which is the convention our look_at/perspective
    // pair produces once the renderer's y-flip is accounted for.
    float ndc_x = (2.0f * mouse_x / screen_w) - 1.0f;
    float ndc_y = 1.0f - (2.0f * mouse_y / screen_h);

    em::mat4 inv_vp = inverse(cam.proj(aspect) * cam.view());

    // Two points along the ray: near plane (z=0 in 0..1 clip) and far (z=1).
    em::vec4 near_clip {ndc_x, ndc_y, 0.0f, 1.0f};
    em::vec4 far_clip  {ndc_x, ndc_y, 1.0f, 1.0f};

    em::vec4 near_w = inv_vp * near_clip;
    em::vec4 far_w  = inv_vp * far_clip;

    // Perspective divide.
    em::vec3 np {near_w.x / near_w.w, near_w.y / near_w.w, near_w.z / near_w.w};
    em::vec3 fp {far_w.x  / far_w.w,  far_w.y  / far_w.w,  far_w.z  / far_w.w};

    Ray r;
    r.origin = np;
    r.dir    = em::normalize(fp - np);
    return r;
}

// ----------------------------------------------------------------------------
//  ray_aabb — slab test against an axis-aligned box [mn, mx].
//  Returns true on hit and writes the nearest non-negative entry distance to t.
// ----------------------------------------------------------------------------
inline bool ray_aabb(const Ray& r, em::vec3 mn, em::vec3 mx, float& t) {
    float tmin = 0.0f;
    float tmax = 1e30f;

    const float ro[3] = {r.origin.x, r.origin.y, r.origin.z};
    const float rd[3] = {r.dir.x,    r.dir.y,    r.dir.z};
    const float bmn[3] = {mn.x, mn.y, mn.z};
    const float bmx[3] = {mx.x, mx.y, mx.z};

    for (int i = 0; i < 3; ++i) {
        if (rd[i] > -1e-8f && rd[i] < 1e-8f) {
            // Ray parallel to this slab: miss if origin outside the slab.
            if (ro[i] < bmn[i] || ro[i] > bmx[i]) return false;
        } else {
            float inv = 1.0f / rd[i];
            float t1 = (bmn[i] - ro[i]) * inv;
            float t2 = (bmx[i] - ro[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    t = tmin;
    return true;
}

// ----------------------------------------------------------------------------
//  ray_sphere — analytic ray/sphere intersection.
//  Returns true on hit and writes the nearest non-negative distance to t.
// ----------------------------------------------------------------------------
inline bool ray_sphere(const Ray& r, em::vec3 c, float radius, float& t) {
    em::vec3 oc = r.origin - c;
    float b = em::dot(oc, r.dir);
    float cc = em::dot(oc, oc) - radius * radius;
    float disc = b * b - cc;
    if (disc < 0.0f) return false;
    float s = std::sqrt(disc);
    float t0 = -b - s;
    float t1 = -b + s;
    if (t0 >= 0.0f)      { t = t0; return true; }
    else if (t1 >= 0.0f) { t = t1; return true; }  // origin inside sphere
    return false;  // sphere entirely behind the ray
}

} // namespace pick

#pragma once
// =============================================================================
//  math.hpp  —  minimal, self-contained linear-algebra for the engine.
//  Column-major matrices, right-handed, depth 0..1 (Vulkan/SDL3-GPU clip space).
// =============================================================================
#include <cmath>
#include <cstdint>

namespace em { // engine math

constexpr float PI  = 3.14159265358979323846f;
constexpr float TAU = 2.0f * PI;
inline float radians(float deg) { return deg * (PI / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / PI); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ----------------------------------------------------------------------------- vec2
struct vec2 {
    float x = 0, y = 0;
    vec2() = default;
    vec2(float x_, float y_) : x(x_), y(y_) {}
};

// ----------------------------------------------------------------------------- vec3
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    vec3(float s) : x(s), y(s), z(s) {}
    vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    vec3 operator+(const vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    vec3 operator-(const vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    vec3 operator-() const { return {-x, -y, -z}; }
    vec3& operator+=(const vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline float dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3  cross(const vec3& a, const vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const vec3& v) { return std::sqrt(dot(v, v)); }
inline vec3  normalize(const vec3& v) {
    float l = length(v);
    return l > 1e-8f ? v * (1.0f / l) : vec3(0, 0, 0);
}

// ----------------------------------------------------------------------------- vec4
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4() = default;
    vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    vec4(const vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

// ----------------------------------------------------------------------------- mat4 (column-major)
struct mat4 {
    // m[col][row]
    float m[4][4] = {};

    mat4() = default;
    static mat4 identity() {
        mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    mat4 operator*(const mat4& b) const {
        mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k][row] * b.m[c][k];
                r.m[c][row] = s;
            }
        return r;
    }

    vec4 operator*(const vec4& v) const {
        return {
            m[0][0]*v.x + m[1][0]*v.y + m[2][0]*v.z + m[3][0]*v.w,
            m[0][1]*v.x + m[1][1]*v.y + m[2][1]*v.z + m[3][1]*v.w,
            m[0][2]*v.x + m[1][2]*v.y + m[2][2]*v.z + m[3][2]*v.w,
            m[0][3]*v.x + m[1][3]*v.y + m[2][3]*v.z + m[3][3]*v.w,
        };
    }
};

inline mat4 translate(const vec3& t) {
    mat4 r = mat4::identity();
    r.m[3][0] = t.x; r.m[3][1] = t.y; r.m[3][2] = t.z;
    return r;
}
inline mat4 scale(const vec3& s) {
    mat4 r = mat4::identity();
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
    return r;
}

// ----------------------------------------------------------------------------- quat
struct quat {
    float x = 0, y = 0, z = 0, w = 1;
    quat() = default;
    quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static quat from_axis_angle(const vec3& axis, float angle) {
        vec3 a = normalize(axis);
        float s = std::sin(angle * 0.5f);
        return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5f)};
    }
    static quat from_euler(float pitch, float yaw, float roll) {
        float cp = std::cos(pitch*0.5f), sp = std::sin(pitch*0.5f);
        float cy = std::cos(yaw*0.5f),   sy = std::sin(yaw*0.5f);
        float cr = std::cos(roll*0.5f),  sr = std::sin(roll*0.5f);
        return {
            sp*cy*cr - cp*sy*sr,
            cp*sy*cr + sp*cy*sr,
            cp*cy*sr - sp*sy*cr,
            cp*cy*cr + sp*sy*sr
        };
    }
};

inline mat4 quat_to_mat4(const quat& q) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    mat4 r = mat4::identity();
    r.m[0][0] = 1 - 2*(yy+zz); r.m[0][1] = 2*(xy+wz);     r.m[0][2] = 2*(xz-wy);
    r.m[1][0] = 2*(xy-wz);     r.m[1][1] = 1 - 2*(xx+zz); r.m[1][2] = 2*(yz+wx);
    r.m[2][0] = 2*(xz+wy);     r.m[2][1] = 2*(yz-wx);     r.m[2][2] = 1 - 2*(xx+yy);
    return r;
}

// TRS compose
inline mat4 compose(const vec3& t, const quat& r, const vec3& s) {
    return translate(t) * quat_to_mat4(r) * scale(s);
}

// ----------------------------------------------------------------------------- camera matrices
inline mat4 look_at(const vec3& eye, const vec3& center, const vec3& up) {
    vec3 f = normalize(center - eye);
    vec3 s = normalize(cross(f, up));
    vec3 u = cross(s, f);
    mat4 r = mat4::identity();
    r.m[0][0] = s.x; r.m[1][0] = s.y; r.m[2][0] = s.z;
    r.m[0][1] = u.x; r.m[1][1] = u.y; r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -dot(s, eye);
    r.m[3][1] = -dot(u, eye);
    r.m[3][2] =  dot(f, eye);
    return r;
}

// Right-handed perspective, depth range 0..1 (Vulkan-style clip).
inline mat4 perspective(float fovy, float aspect, float znear, float zfar) {
    float tan_half = std::tan(fovy * 0.5f);
    mat4 r;
    r.m[0][0] = 1.0f / (aspect * tan_half);
    r.m[1][1] = 1.0f / tan_half;
    r.m[2][2] = zfar / (znear - zfar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (zfar * znear) / (znear - zfar);
    return r;
}

// Orthographic for 2D UI, depth 0..1.
inline mat4 ortho(float left, float right, float bottom, float top) {
    mat4 r = mat4::identity();
    r.m[0][0] = 2.0f / (right - left);
    r.m[1][1] = 2.0f / (top - bottom);
    r.m[2][2] = -1.0f;
    r.m[3][0] = -(right + left) / (right - left);
    r.m[3][1] = -(top + bottom) / (top - bottom);
    return r;
}

// Right-handed orthographic with an explicit depth range, mapping view-space
// z in [-near, -far] to clip z in [0, 1] (Vulkan-style). Used for shadow maps.
inline mat4 ortho(float left, float right, float bottom, float top, float znear, float zfar) {
    mat4 r = mat4::identity();
    r.m[0][0] = 2.0f / (right - left);
    r.m[1][1] = 2.0f / (top - bottom);
    r.m[2][2] = -1.0f / (zfar - znear);
    r.m[3][0] = -(right + left) / (right - left);
    r.m[3][1] = -(top + bottom) / (top - bottom);
    r.m[3][2] = -znear / (zfar - znear);
    return r;
}

} // namespace em

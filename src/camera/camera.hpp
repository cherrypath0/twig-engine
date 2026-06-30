#pragma once
// Free-fly perspective camera. Pure math — input is fed in by the engine so this
// header stays free of any SDL dependency.
#include "math.hpp"

struct Camera {
    em::vec3 position {0.0f, 3.0f, 9.0f};
    float yaw   = -90.0f;   // degrees, around +Y
    float pitch = -12.0f;   // degrees
    float fov_deg = 60.0f;
    float znear = 0.05f;
    float zfar  = 2000.0f;
    float move_speed = 7.0f;
    float look_sens  = 0.12f;

    em::vec3 forward() const {
        float cy = std::cos(em::radians(yaw)),   sy = std::sin(em::radians(yaw));
        float cp = std::cos(em::radians(pitch)), sp = std::sin(em::radians(pitch));
        return em::normalize({cy * cp, sp, sy * cp});
    }
    em::vec3 right() const { return em::normalize(em::cross(forward(), {0, 1, 0})); }
    em::vec3 up()    const { return em::cross(right(), forward()); }

    em::mat4 view() const { return em::look_at(position, position + forward(), {0, 1, 0}); }
    em::mat4 proj(float aspect) const {
        return em::perspective(em::radians(fov_deg), aspect, znear, zfar);
    }

    // dx, dy are mouse deltas in pixels.
    void add_look(float dx, float dy) {
        yaw   += dx * look_sens;
        pitch  = em::clampf(pitch - dy * look_sens, -89.0f, 89.0f);
    }
    // local_dir components: x=right, y=up, z=forward, each in [-1,1].
    void move(const em::vec3& local_dir, float dt, float boost = 1.0f) {
        em::vec3 delta = right() * local_dir.x + em::vec3{0, 1, 0} * local_dir.y + forward() * local_dir.z;
        position += delta * (move_speed * boost * dt);
    }
};

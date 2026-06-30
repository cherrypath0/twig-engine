#pragma once
// Physics façade over Jolt Physics. No Jolt types leak through this header, so
// the rest of the engine compiles without Jolt's include path (and the Windows
// build can use the no-op stub). See physics.cpp for both implementations.
#include "math.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

struct PhysicsBody {
    uint32_t id = 0xFFFFFFFFu;
    bool valid() const { return id != 0xFFFFFFFFu; }
};

class PhysicsWorld {
public:
    bool init();
    void shutdown();
    void update(float dt);
    void optimize();                       // call once after building the scene

    PhysicsBody add_box(const em::vec3& half_extent, const em::vec3& pos, bool dynamic);
    PhysicsBody add_sphere(float radius, const em::vec3& pos, bool dynamic);
    // Capsule aligned on the Y axis: total height = 2*half_height + 2*radius.
    PhysicsBody add_capsule(float radius, float half_height, const em::vec3& pos, bool dynamic);
    // Convex hull built from an arbitrary point cloud (e.g. mesh vertices).
    PhysicsBody add_convex(const std::vector<em::vec3>& points, const em::vec3& pos, bool dynamic);

    // Best-effort removal: detaches & destroys the Jolt body and marks the slot
    // invalid so later index lookups are skipped. Indices of other bodies are
    // preserved (the slot is tombstoned, not erased).
    void remove_body(PhysicsBody b);

    void get_transform(PhysicsBody b, em::vec3& out_pos, em::quat& out_rot);
    void set_position(PhysicsBody b, const em::vec3& pos);
    void apply_impulse(PhysicsBody b, const em::vec3& impulse);
    void set_gravity(const em::vec3& g);

    bool   enabled() const { return enabled_; }
    size_t body_count() const;
    const char* backend() const;           // "Jolt" or "stub"

private:
    bool  enabled_ = false;
    void* impl_ = nullptr;
};

#include "pch.hpp"
#include "physics/physics.hpp"

// =============================================================================
//  Real implementation — Jolt Physics
// =============================================================================
#ifdef ENGINE_WITH_JOLT

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>

#include <thread>
#include <algorithm>
#include <vector>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

// --- object / broadphase layers ---------------------------------------------
namespace Layers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}
namespace BPLayers {
    static constexpr BroadPhaseLayer NON_MOVING{0};
    static constexpr BroadPhaseLayer MOVING{1};
    static constexpr uint NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        map_[Layers::NON_MOVING] = BPLayers::NON_MOVING;
        map_[Layers::MOVING]     = BPLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return map_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer) const override { return "layer"; }
#endif
private:
    BroadPhaseLayer map_[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer o, BroadPhaseLayer b) const override {
        if (o == Layers::NON_MOVING) return b == BPLayers::MOVING;
        return true; // MOVING collides with everything
    }
};
class ObjectPairFilter final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true;
    }
};

struct PhysicsImpl {
    TempAllocatorImpl*      temp = nullptr;
    JobSystemThreadPool*    jobs = nullptr;
    PhysicsSystem           system;
    BPLayerInterfaceImpl    bp_layer_iface;
    ObjectVsBroadPhaseFilter obj_vs_bp;
    ObjectPairFilter        obj_pair;
    std::vector<BodyID>     bodies;
};

bool PhysicsWorld::init() {
    RegisterDefaultAllocator();
    if (Factory::sInstance == nullptr) Factory::sInstance = new Factory();
    RegisterTypes();

    auto* impl = new PhysicsImpl();
    impl->temp = new TempAllocatorImpl(16 * 1024 * 1024);
    int threads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    impl->jobs = new JobSystemThreadPool(2048, 8, threads);

    impl->system.Init(/*maxBodies*/ 8192, /*numBodyMutexes*/ 0,
                      /*maxBodyPairs*/ 8192, /*maxContactConstraints*/ 4096,
                      impl->bp_layer_iface, impl->obj_vs_bp, impl->obj_pair);
    impl->system.SetGravity(Vec3(0.0f, -9.81f, 0.0f));

    impl_ = impl;
    enabled_ = true;
    println("Jolt Physics initialized (%d worker threads)", threads);
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    BodyInterface& bi = impl->system.GetBodyInterface();
    for (BodyID id : impl->bodies) {
        if (!id.IsInvalid()) { bi.RemoveBody(id); bi.DestroyBody(id); }
    }
    delete impl->jobs;
    delete impl->temp;
    delete impl;
    impl_ = nullptr;
    enabled_ = false;
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;
}

void PhysicsWorld::optimize() {
    if (!impl_) return;
    static_cast<PhysicsImpl*>(impl_)->system.OptimizeBroadPhase();
}

void PhysicsWorld::update(float dt) {
    if (!impl_) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (dt <= 0.0f) return;
    impl->system.Update(dt, /*collisionSteps*/ 1, impl->temp, impl->jobs);
}

static PhysicsBody push_body(PhysicsImpl* impl, const ShapeRefC& shape,
                             const em::vec3& pos, bool dynamic) {
    BodyInterface& bi = impl->system.GetBodyInterface();
    EMotionType motion = dynamic ? EMotionType::Dynamic : EMotionType::Static;
    ObjectLayer layer  = dynamic ? Layers::MOVING : Layers::NON_MOVING;
    BodyCreationSettings bcs(shape, RVec3(pos.x, pos.y, pos.z),
                             Quat::sIdentity(), motion, layer);
    BodyID id = bi.CreateAndAddBody(bcs, dynamic ? EActivation::Activate
                                                 : EActivation::DontActivate);
    impl->bodies.push_back(id);
    return PhysicsBody{ static_cast<uint32_t>(impl->bodies.size() - 1) };
}

PhysicsBody PhysicsWorld::add_box(const em::vec3& half, const em::vec3& pos, bool dynamic) {
    if (!impl_) return {};
    BoxShapeSettings settings(Vec3(half.x, half.y, half.z));
    settings.SetEmbedded();
    ShapeRefC shape = settings.Create().Get();
    return push_body(static_cast<PhysicsImpl*>(impl_), shape, pos, dynamic);
}

PhysicsBody PhysicsWorld::add_sphere(float radius, const em::vec3& pos, bool dynamic) {
    if (!impl_) return {};
    SphereShapeSettings settings(radius);
    settings.SetEmbedded();
    ShapeRefC shape = settings.Create().Get();
    return push_body(static_cast<PhysicsImpl*>(impl_), shape, pos, dynamic);
}

PhysicsBody PhysicsWorld::add_capsule(float radius, float half_height,
                                      const em::vec3& pos, bool dynamic) {
    if (!impl_) return {};
    // Jolt's CapsuleShape is Y-aligned: inHalfHeightOfCylinder + inRadius.
    CapsuleShapeSettings settings(half_height, radius);
    settings.SetEmbedded();
    ShapeRefC shape = settings.Create().Get();
    return push_body(static_cast<PhysicsImpl*>(impl_), shape, pos, dynamic);
}

PhysicsBody PhysicsWorld::add_convex(const std::vector<em::vec3>& points,
                                     const em::vec3& pos, bool dynamic) {
    if (!impl_ || points.empty()) return {};
    // Build a Jolt point list from the supplied cloud, then let Jolt compute the
    // convex hull. Create() can fail (e.g. degenerate/coplanar input); on failure
    // we return an invalid body rather than crash.
    Array<Vec3> hull_points;
    hull_points.reserve(points.size());
    for (const em::vec3& p : points)
        hull_points.push_back(Vec3(p.x, p.y, p.z));

    ConvexHullShapeSettings settings(hull_points);
    settings.SetEmbedded();
    ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return {};
    ShapeRefC shape = result.Get();
    return push_body(static_cast<PhysicsImpl*>(impl_), shape, pos, dynamic);
}

void PhysicsWorld::remove_body(PhysicsBody b) {
    if (!impl_ || !b.valid()) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (b.id >= impl->bodies.size()) return;
    BodyID id = impl->bodies[b.id];
    if (id.IsInvalid()) return;             // already removed
    BodyInterface& bi = impl->system.GetBodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    // Tombstone the slot so indices of other bodies stay stable. shutdown()
    // already skips invalid IDs.
    impl->bodies[b.id] = BodyID();
}

void PhysicsWorld::get_transform(PhysicsBody b, em::vec3& out_pos, em::quat& out_rot) {
    if (!impl_ || !b.valid()) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (b.id >= impl->bodies.size()) return;
    BodyID id = impl->bodies[b.id];
    if (id.IsInvalid()) return;            // tombstoned by remove_body
    BodyInterface& bi = impl->system.GetBodyInterface();
    RVec3 p = bi.GetPosition(id);
    Quat  q = bi.GetRotation(id);
    out_pos = { (float)p.GetX(), (float)p.GetY(), (float)p.GetZ() };
    out_rot = { q.GetX(), q.GetY(), q.GetZ(), q.GetW() };
}

void PhysicsWorld::set_position(PhysicsBody b, const em::vec3& pos) {
    if (!impl_ || !b.valid()) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (b.id >= impl->bodies.size()) return;
    if (impl->bodies[b.id].IsInvalid()) return;
    impl->system.GetBodyInterface().SetPosition(impl->bodies[b.id],
        RVec3(pos.x, pos.y, pos.z), EActivation::Activate);
}

void PhysicsWorld::apply_impulse(PhysicsBody b, const em::vec3& impulse) {
    if (!impl_ || !b.valid()) return;
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (b.id >= impl->bodies.size()) return;
    if (impl->bodies[b.id].IsInvalid()) return;
    impl->system.GetBodyInterface().AddImpulse(impl->bodies[b.id],
        Vec3(impulse.x, impulse.y, impulse.z));
}

void PhysicsWorld::set_gravity(const em::vec3& g) {
    if (!impl_) return;
    static_cast<PhysicsImpl*>(impl_)->system.SetGravity(Vec3(g.x, g.y, g.z));
}

size_t PhysicsWorld::body_count() const {
    return impl_ ? static_cast<PhysicsImpl*>(impl_)->bodies.size() : 0;
}
const char* PhysicsWorld::backend() const { return "Jolt"; }

// =============================================================================
//  Stub implementation — no Jolt available (e.g. Windows cross build)
// =============================================================================
#else

#include <vector>
struct StubBody { em::vec3 pos; em::quat rot; em::vec3 vel; bool dynamic; };
struct PhysicsImpl { std::vector<StubBody> bodies; em::vec3 gravity{0,-9.81f,0}; };

bool PhysicsWorld::init() { impl_ = new PhysicsImpl(); enabled_ = true;
    println("Physics: stub backend (Jolt not compiled in)"); return true; }
void PhysicsWorld::shutdown() { delete static_cast<PhysicsImpl*>(impl_); impl_ = nullptr; enabled_ = false; }
void PhysicsWorld::optimize() {}
void PhysicsWorld::update(float dt) {
    auto* impl = static_cast<PhysicsImpl*>(impl_); if (!impl) return;
    for (auto& b : impl->bodies) if (b.dynamic) { b.vel += impl->gravity * dt; b.pos += b.vel * dt;
        if (b.pos.y < 0) { b.pos.y = 0; b.vel = {}; } }
}
static PhysicsBody stub_add(PhysicsImpl* impl, const em::vec3& pos, bool dyn) {
    impl->bodies.push_back({pos, {}, {}, dyn});
    return PhysicsBody{ static_cast<uint32_t>(impl->bodies.size() - 1) };
}
PhysicsBody PhysicsWorld::add_box(const em::vec3&, const em::vec3& p, bool d) { return stub_add(static_cast<PhysicsImpl*>(impl_), p, d); }
PhysicsBody PhysicsWorld::add_sphere(float, const em::vec3& p, bool d) { return stub_add(static_cast<PhysicsImpl*>(impl_), p, d); }
PhysicsBody PhysicsWorld::add_capsule(float, float, const em::vec3& p, bool d) { return stub_add(static_cast<PhysicsImpl*>(impl_), p, d); }
PhysicsBody PhysicsWorld::add_convex(const std::vector<em::vec3>&, const em::vec3& p, bool d) { return stub_add(static_cast<PhysicsImpl*>(impl_), p, d); }
void PhysicsWorld::remove_body(PhysicsBody b) {
    // Stub keeps slots stable (no Jolt body to destroy). Mark as non-dynamic so
    // it stops integrating; the slot index remains valid for other bodies.
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (impl && b.valid() && b.id < impl->bodies.size()) impl->bodies[b.id].dynamic = false;
}
void PhysicsWorld::get_transform(PhysicsBody b, em::vec3& op, em::quat& orr) {
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (impl && b.valid() && b.id < impl->bodies.size()) { op = impl->bodies[b.id].pos; orr = impl->bodies[b.id].rot; }
}
void PhysicsWorld::set_position(PhysicsBody b, const em::vec3& p) {
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (impl && b.valid() && b.id < impl->bodies.size()) impl->bodies[b.id].pos = p;
}
void PhysicsWorld::apply_impulse(PhysicsBody b, const em::vec3& imp) {
    auto* impl = static_cast<PhysicsImpl*>(impl_);
    if (impl && b.valid() && b.id < impl->bodies.size()) impl->bodies[b.id].vel += imp;
}
void PhysicsWorld::set_gravity(const em::vec3& g) { if (impl_) static_cast<PhysicsImpl*>(impl_)->gravity = g; }
size_t PhysicsWorld::body_count() const { return impl_ ? static_cast<PhysicsImpl*>(impl_)->bodies.size() : 0; }
const char* PhysicsWorld::backend() const { return "stub"; }

#endif

#pragma once
// Scene graph: a flat list of entities, each referencing a GPU mesh + a named
// material + an optional physics body. Source 2 calls these "scene objects";
// we keep it deliberately simple and data-oriented.
#include "math.hpp"
#include "gltf/model.hpp"
#include "physics/physics.hpp"
#include <string>
#include <vector>

struct Transform {
    em::vec3 position {0, 0, 0};
    em::quat rotation {};
    em::vec3 scale    {1, 1, 1};
    em::mat4 matrix() const { return em::compose(position, rotation, scale); }
};

// What role an entity plays in the world. Drives editor UI, rendering and
// physics-body generation. Defaults to Renderable so existing scenes (which
// only set mesh/material) keep behaving exactly as before.
enum class InstanceKind {
    Empty,           // pure transform node (no mesh, no body)
    Renderable,      // drawn, no physics
    RigidBody,       // drawn + dynamic physics body
    StaticCollider,  // static (immovable) physics body, optionally drawn
    Camera           // a placeable camera (transform + fov), not drawn
};

// Shape used when generating a physics collider for an entity. None means the
// entity has no collider yet. Box/Sphere/Capsule are derived from the mesh
// bounds; Convex requires the source CPU vertices (see integrator notes).
enum class ColliderType {
    None,
    Box,
    Sphere,
    Capsule,
    Convex
};

struct Entity {
    std::string  name = "entity";
    Transform    transform;
    int          mesh = -1;             // index into Scene::meshes (-1 = none)
    std::string  material = "default";  // material registry key
    PhysicsBody  body;                  // invalid() == no physics
    bool         visible = true;
    float        fov     = 60.0f;       // camera instances: vertical field of view (deg)

    // --- instance / collider classification (added; existing fields above
    //     are intentionally preserved for backward compatibility) ----------
    InstanceKind kind     = InstanceKind::Renderable;
    ColliderType collider = ColliderType::None;
    std::string  script;                // gameplay script (.cpp stem); empty = none
    int          parent   = -1;         // hierarchy: index of parent entity (-1 = root)

    // Convenience predicates used by the editor and engine.
    bool is_dynamic()    const { return kind == InstanceKind::RigidBody; }
    bool has_collider()  const { return collider != ColliderType::None; }
    bool has_mesh()      const { return mesh >= 0; }
    bool has_script()    const { return !script.empty(); }
};

struct Scene {
    std::vector<GpuMesh> meshes;       // owned GPU geometry
    std::vector<Entity>  entities;

    em::vec3 sun_dir   = em::normalize(em::vec3{-0.4f, -1.0f, -0.35f});
    em::vec3 sky_color {0.10f, 0.12f, 0.16f};

    int add_mesh(const GpuMesh& m) {
        meshes.push_back(m);
        return static_cast<int>(meshes.size()) - 1;
    }
    Entity& add_entity(const Entity& e) {
        entities.push_back(e);
        return entities.back();
    }

    // World transform = parent_world * ... * local. Walks up the parent chain
    // (guarded against cycles). Used everywhere a child must follow its parent.
    em::mat4 world_matrix(int i) const {
        if (i < 0 || i >= (int)entities.size()) return em::mat4::identity();
        em::mat4 m = entities[i].transform.matrix();
        int p = entities[i].parent, guard = 0;
        while (p >= 0 && p < (int)entities.size() && guard++ < 64) {
            m = entities[p].transform.matrix() * m;
            p = entities[p].parent;
        }
        return m;
    }
};

#pragma once
// The Engine owns the window, GPU device and every subsystem, and drives the
// main loop: input -> physics -> UI -> render.
#include "renderer/render.hpp"
#include "scene/scene.hpp"
#include "camera/camera.hpp"
#include "physics/physics.hpp"
#include "material/material.hpp"
#include "project/project.hpp"
#include "gltf/model.hpp"
#include "ui/nuklear_backend.hpp"
#include "ui/editor.hpp"
#include "ui/icons.hpp"
#include "ui/gizmo.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;

class Engine {
public:
    bool init();
    void run();
    void shutdown();

private:
    void load_materials();
    void build_default_scene();
    void try_load_glb(const std::string& path);
    void reset_scene();
    void spawn_box(const em::vec3& pos);
    void spawn_sphere(const em::vec3& pos);

    // Upload a mesh AND retain its CPU MeshData so convex hulls can be built
    // later. Keeps mesh_cpu_ index-aligned with scene_.meshes.
    int  add_mesh(const MeshData& md);

    // editor-driven actions
    em::vec3 spawn_point() const;            // a little in front of the camera
    void spawn_primitive(int kind);          // 0 cube, 1 sphere, 2 plane
    void spawn_instance(int kind);           // InstanceKind: Empty/Renderable/RigidBody/StaticCollider
    void instantiate_model(const std::string& path);
    void import_dropped_file(const std::string& src);   // drag & drop from the file manager
    std::string material_for_primitive(const Primitive& prim);  // GLB base color -> material
    void save_scene(const std::string& path);
    void load_scene(const std::string& path);
    std::string scene_path() const;
    int  glb_mesh(const std::string& path, int idx);   // load (cached) + return mesh index
    void assign_material(const std::string& name);
    void assign_script(const std::string& stem);
    void set_kind(int kind);                  // change selected entity's InstanceKind
    void generate_collider(int collider_type);// build a Jolt collider from the mesh
    void delete_entity(int index);            // remove entity + its physics body
    void delete_selected();
    void clone_selected();                    // duplicate the selection + select the copy

    void handle_input(float dt);
    void process_editor_actions();
    void update_gizmo();                       // run the transform gizmo for the selection
    void pick_entity(float mx, float my);      // ray-pick the nearest entity under the cursor
    void sync_physics();
    void toggle_play();              // start/stop in-editor simulation

    SDL_Window*    window_ = nullptr;
    SDL_GPUDevice* gpu_    = nullptr;

    Renderer       renderer_;
    NuklearBackend ui_;
    Editor         editor_;
    IconSet        icons_;
    Gizmo          gizmo_;
    Scene          scene_;
    Camera         camera_;
    PhysicsWorld   physics_;
    Project        project_;
    std::unordered_map<std::string, Material> materials_;
    std::unordered_map<int, std::string> glb_mesh_src_;            // mesh index -> "glb:path:idx"
    std::unordered_map<std::string, std::vector<int>> glb_cache_;  // path -> mesh indices

    // CPU-side mesh geometry kept index-aligned with scene_.meshes, so the
    // editor can generate convex-hull colliders from the original vertices
    // (GpuMesh keeps only the GPU buffers + bounds).
    std::vector<MeshData> mesh_cpu_;

    int  cube_mesh_   = -1;
    int  sphere_mesh_ = -1;
    int  plane_mesh_  = -1;

    bool running_        = true;
    bool mouse_look_     = false;
    bool window_claimed_ = false;
    bool playing_        = false;    // false = EDIT mode (physics paused)
    float vp_x_ = 0, vp_y_ = 0, vp_w_ = 0, vp_h_ = 0;   // 3D viewport rect (editor center)
    std::vector<Transform> play_snapshot_;   // entity transforms saved at Play
    float spawn_clock_   = 0.0f;

    // Gizmo / picking state. `gizmo_active_` is set each frame by update_gizmo()
    // and consumed by apply_camera() to suppress camera movement during a drag.
    bool  gizmo_active_   = false;
    int   gizmo_hot_      = -1;   // hovered/active gizmo handle (for yellow highlight)
    bool  left_was_down_  = false;  // edge-detect left clicks for ray-picking
    bool  click_pending_  = false;  // a fresh left-press happened this frame
    float look_dx_        = 0.0f;   // accumulated mouse-look delta (right-drag)
    float look_dy_        = 0.0f;

    void apply_camera(float dt);    // move/look the camera (skipped during a drag)
};

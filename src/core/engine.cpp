#include "pch.hpp"
#include "core/engine.hpp"
#include "core/picking.hpp"   // pick::screen_ray / ray_sphere for click selection
#include "gltf/model.hpp"
#include "gpu/gpu.hpp"

#include <algorithm>          // std::max
#include <cmath>
#include <cstdio>              // std::sqrt
#include <filesystem>
#include <fstream>
#include <sstream>

// ----------------------------------------------------------------------------- init
bool Engine::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        warnln("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    println("Video driver: %s", SDL_GetCurrentVideoDriver());

    window_ = SDL_CreateWindow("Twig Engine", 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) { warnln("SDL_CreateWindow failed: %s", SDL_GetError()); return false; }

    // Window / taskbar icon from the Twig branding mark.
    if (SDL_Surface* icon = rasterize_svg_surface("assets/Branding/DarkMode.svg", 256)) {
        SDL_SetWindowIcon(window_, icon);
        SDL_DestroySurface(icon);
    }

    // Vulkan validation layers are great for debugging but some driver stacks
    // double-free a validation allocation during SDL_Quit, so keep them opt-in.
    const bool gpu_debug = SDL_getenv("ENGINE_GPU_DEBUG") != nullptr;
    gpu_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, gpu_debug, "vulkan");
    if (!gpu_) { warnln("SDL_CreateGPUDevice failed: %s", SDL_GetError()); return false; }
    println("Renderer backend: %s", SDL_GetGPUDeviceDriver(gpu_));

    if (!SDL_ClaimWindowForGPUDevice(gpu_, window_)) {
        warnln("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }
    window_claimed_ = true;
    SDL_StartTextInput(window_);   // SDL3: text input is OFF by default;
                                   // enable it so the code editor + fields receive chars.

    if (!renderer_.init(gpu_, window_)) { warnln("Renderer init failed"); return false; }
    if (!ui_.init(gpu_, window_, renderer_.swapchain_format())) { warnln("UI init failed"); return false; }
    editor_.init();
    editor_.set_mono_font(ui_.mono_font());
    physics_.init();

    // SVG editor icons: load AFTER the GPU device and Nuklear backend exist (the
    // icons share Nuklear's image handles and are uploaded as GPU textures).
    if (!icons_.init(gpu_, "assets/icons", 48))
        editor_.log("[icons] failed to initialise icon set (using fallbacks)");

    // Open (or scaffold) the game project — content lives THERE, not in the engine.
    project_.open("");
    editor_.log("Project: " + project_.root());

    load_materials();
    build_default_scene();
    physics_.optimize();
    if (std::filesystem::exists(scene_path())) load_scene(scene_path());

    editor_.log("Engine initialised. Physics backend: " + std::string(physics_.backend()));
    return true;
}

// ----------------------------------------------------------------------------- materials
void Engine::load_materials() {
    materials_.clear();
    // Always provide a fallback "default".
    Material def = material::make_default();
    def.name = "default";
    material::resolve_gpu(gpu_, def, renderer_.white());
    materials_["default"] = def;

    // Everything else comes from the PROJECT's assets/materials/*.tgmat.
    for (const AssetEntry& a : project_.materials()) {
        Material m;
        if (!material::load_tgmat(a.path, m)) continue;
        m.name = a.stem;
        material::resolve_gpu(gpu_, m, renderer_.white());
        materials_[a.stem] = m;
    }
    editor_.log("Loaded " + std::to_string(materials_.size()) + " materials");
}

// ----------------------------------------------------------------------------- scene
// Upload a mesh and cache its CPU geometry so the index stays aligned with
// scene_.meshes and convex colliders can be rebuilt from the source vertices.
int Engine::add_mesh(const MeshData& md) {
    int idx = scene_.add_mesh(model::upload(gpu_, md));
    if ((int)mesh_cpu_.size() <= idx) mesh_cpu_.resize(idx + 1);
    mesh_cpu_[idx] = md;
    return idx;
}

void Engine::build_default_scene() {
    cube_mesh_   = add_mesh(model::make_cube(0.5f));
    sphere_mesh_ = add_mesh(model::make_sphere(0.5f, 24));
    plane_mesh_  = add_mesh(model::make_plane(20.0f, 20.0f));

    Entity floor;
    floor.name = "floor";
    floor.mesh = plane_mesh_;
    floor.material = "grid";
    floor.kind = InstanceKind::StaticCollider;
    scene_.add_entity(floor);
    // static collider for the floor (not bound to the visual entity, so it is
    // never overwritten by physics sync)
    physics_.add_box({20.0f, 0.5f, 20.0f}, {0.0f, -0.5f, 0.0f}, false);

    for (int i = 0; i < 6; ++i)
        spawn_box({0.0f, 1.0f + i * 1.25f, 0.0f});

    spawn_sphere({2.5f, 6.0f, 0.5f});

    // Auto-load every model that lives in the project's assets/models/.
    for (const AssetEntry& a : project_.models()) try_load_glb(a.path);
}

void Engine::spawn_box(const em::vec3& pos) {
    Entity e;
    e.name = "box_" + std::to_string(scene_.entities.size());
    e.mesh = cube_mesh_;
    e.material = "crate";
    e.transform.position = pos;
    e.kind = InstanceKind::RigidBody;
    e.collider = ColliderType::Box;
    e.body = physics_.add_box({0.5f, 0.5f, 0.5f}, pos, true);
    scene_.add_entity(e);
}

void Engine::spawn_sphere(const em::vec3& pos) {
    Entity e;
    e.name = "sphere_" + std::to_string(scene_.entities.size());
    e.mesh = sphere_mesh_;
    e.material = "default";
    e.transform.position = pos;
    e.kind = InstanceKind::RigidBody;
    e.collider = ColliderType::Sphere;
    e.body = physics_.add_sphere(0.5f, pos, true);
    scene_.add_entity(e);
}

void Engine::try_load_glb(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        editor_.log("[glb] no " + path + " (drop one in to load it)");
        return;
    }
    Model mdl;
    if (!model::load_glb(path, mdl)) return;
    std::string gnm = std::filesystem::path(path).stem().string();
    Entity grp; grp.name = gnm; grp.kind = InstanceKind::Empty; grp.mesh = -1;
    grp.transform.position = {-4.0f, 1.0f, -4.0f};
    int pi = (int)scene_.entities.size(); scene_.add_entity(grp);
    int gi = 0;
    for (const Primitive& prim : mdl.primitives) {
        int mesh = add_mesh(prim.mesh);
        glb_mesh_src_[mesh] = "glb:" + path + ":" + std::to_string(gi);
        Entity e;
        e.name = gnm + "_part" + std::to_string(gi++);
        e.mesh = mesh;
        e.material = material_for_primitive(prim);
        e.parent = pi;                          // child -> the group keeps it together
        scene_.add_entity(e);                   // local transform identity (offsets are baked)
    }
    editor_.log("[glb] loaded " + path);
}

void Engine::reset_scene() {
    for (GpuMesh& m : scene_.meshes) model::destroy(gpu_, m);
    scene_ = Scene{};
    mesh_cpu_.clear();
    physics_.shutdown();
    physics_.init();
    editor_.set_selected(-1);
    build_default_scene();
    physics_.optimize();
    editor_.log("[scene] reset");
}

std::string Engine::scene_path() const { return project_.dir_scenes() + "/main.twigscene"; }

int Engine::glb_mesh(const std::string& path, int idx) {
    auto it = glb_cache_.find(path);
    if (it == glb_cache_.end()) {
        std::vector<int> idxs;
        Model mdl;
        if (model::load_glb(path, mdl))
            for (const Primitive& prim : mdl.primitives) {
                int m = add_mesh(prim.mesh);
                material_for_primitive(prim);            // recreate material so saved refs resolve
                glb_mesh_src_[m] = "glb:" + path + ":" + std::to_string(idxs.size());
                idxs.push_back(m);
            }
        it = glb_cache_.emplace(path, std::move(idxs)).first;
    }
    return (idx >= 0 && idx < (int)it->second.size()) ? it->second[idx] : -1;
}

void Engine::save_scene(const std::string& path) {
    std::error_code ec; std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(path);
    if (!f) { editor_.log("[scene] save failed: " + path); return; }
    f << "twigscene 1\n";
    for (const Entity& e : scene_.entities) {
        std::string src = "none";
        if (e.mesh == cube_mesh_) src = "cube";
        else if (e.mesh == sphere_mesh_) src = "sphere";
        else if (e.mesh == plane_mesh_) src = "plane";
        else { auto it = glb_mesh_src_.find(e.mesh); if (it != glb_mesh_src_.end()) src = it->second; }
        const Transform& t = e.transform;
        f << "entity\nname " << e.name << "\nmesh " << src << "\n";
        f << "pos " << t.position.x << ' ' << t.position.y << ' ' << t.position.z << "\n";
        f << "rot " << t.rotation.x << ' ' << t.rotation.y << ' ' << t.rotation.z << ' ' << t.rotation.w << "\n";
        f << "scl " << t.scale.x << ' ' << t.scale.y << ' ' << t.scale.z << "\n";
        f << "par " << e.parent << "\n";
        f << "mat " << e.material << "\nkind " << (int)e.kind << "\ncol " << (int)e.collider
          << "\nfov " << e.fov << "\nvis " << (e.visible ? 1 : 0) << "\n";
        if (!e.script.empty()) f << "script " << e.script << "\n";
        f << "end\n";
    }
    editor_.log("[scene] saved " + path);
}

void Engine::load_scene(const std::string& path) {
    std::ifstream f(path);
    if (!f) { editor_.log("[scene] no saved scene at " + path); return; }
    std::string magic; int ver = 0; f >> magic >> ver;
    if (magic != "twigscene") { editor_.log("[scene] not a scene file"); return; }
    // reset everything, keep the primitive meshes available
    for (GpuMesh& m : scene_.meshes) model::destroy(gpu_, m);
    scene_ = Scene{}; mesh_cpu_.clear(); glb_mesh_src_.clear(); glb_cache_.clear();
    physics_.shutdown(); physics_.init();
    cube_mesh_   = add_mesh(model::make_cube(0.5f));
    sphere_mesh_ = add_mesh(model::make_sphere(0.5f, 24));
    plane_mesh_  = add_mesh(model::make_plane(20.0f, 20.0f));
    std::string line, mesh_src = "none"; Entity cur; bool in_e = false;
    while (std::getline(f, line)) {
        std::istringstream ls(line); std::string key; ls >> key;
        if (key == "entity") { cur = Entity{}; in_e = true; mesh_src = "none"; }
        else if (key == "name") { std::getline(ls >> std::ws, cur.name); }
        else if (key == "mesh") { std::getline(ls >> std::ws, mesh_src); }
        else if (key == "pos") { ls >> cur.transform.position.x >> cur.transform.position.y >> cur.transform.position.z; }
        else if (key == "rot") { ls >> cur.transform.rotation.x >> cur.transform.rotation.y >> cur.transform.rotation.z >> cur.transform.rotation.w; }
        else if (key == "scl") { ls >> cur.transform.scale.x >> cur.transform.scale.y >> cur.transform.scale.z; }
        else if (key == "mat") { ls >> cur.material; }
        else if (key == "par") { ls >> cur.parent; }
        else if (key == "kind") { int k = 0; ls >> k; cur.kind = (InstanceKind)k; }
        else if (key == "col") { int c = 0; ls >> c; cur.collider = (ColliderType)c; }
        else if (key == "fov") { ls >> cur.fov; }
        else if (key == "vis") { int v = 1; ls >> v; cur.visible = (v != 0); }
        else if (key == "script") { ls >> cur.script; }
        else if (key == "end" && in_e) {
            if (mesh_src == "cube") cur.mesh = cube_mesh_;
            else if (mesh_src == "sphere") cur.mesh = sphere_mesh_;
            else if (mesh_src == "plane") cur.mesh = plane_mesh_;
            else if (mesh_src.rfind("glb:", 0) == 0) {
                size_t c1 = mesh_src.find(':'), c2 = mesh_src.rfind(':');
                cur.mesh = glb_mesh(mesh_src.substr(c1 + 1, c2 - c1 - 1), std::atoi(mesh_src.substr(c2 + 1).c_str()));
            } else cur.mesh = -1;
            if (cur.collider != ColliderType::None) {
                bool dyn = (cur.kind == InstanceKind::RigidBody);
                em::vec3 s = cur.transform.scale, half{0.5f * s.x, 0.5f * s.y, 0.5f * s.z};
                if (cur.mesh >= 0 && cur.mesh < (int)scene_.meshes.size()) {
                    em::vec3 ext = (scene_.meshes[cur.mesh].bounds_max - scene_.meshes[cur.mesh].bounds_min) * 0.5f;
                    half = {std::max(0.02f, ext.x * s.x), std::max(0.02f, ext.y * s.y), std::max(0.02f, ext.z * s.z)};
                }
                if (cur.collider == ColliderType::Sphere) cur.body = physics_.add_sphere(std::max({half.x, half.y, half.z}), cur.transform.position, dyn);
                else cur.body = physics_.add_box(half, cur.transform.position, dyn);
            }
            scene_.add_entity(cur); in_e = false;
        }
    }
    physics_.optimize();
    editor_.set_selected(-1);
    editor_.log("[scene] loaded " + path);
}

// ----------------------------------------------------------------------------- input
// Poll SDL events and accumulate per-frame input state. Camera MOVEMENT is NOT
// applied here — it is deferred to apply_camera() so the gizmo (built later
// during UI) can veto it. W/E/R switch the gizmo mode, but only when NOT in
// fly-look mode (right mouse held) so they don't collide with WASD movement.
void Engine::handle_input(float dt) {
    (void)dt;
    ui_.input_begin();
    look_dx_ = 0.0f; look_dy_ = 0.0f;
    click_pending_ = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        ui_.handle_event(e);
        switch (e.type) {
            case SDL_EVENT_QUIT: running_ = false; break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouse_look_) { look_dx_ += e.motion.xrel; look_dy_ += e.motion.yrel; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    // Enter fly-cam ONLY when right-clicking in the 3D viewport
                    // (never on a UI panel); lock the mouse while flying.
                    const float dens = SDL_GetWindowPixelDensity(window_);
                    const float mx = e.button.x * dens, my = e.button.y * dens;
                    if (mx >= vp_x_ && mx < vp_x_ + vp_w_ &&
                        my >= vp_y_ && my < vp_y_ + vp_h_ && !editor_.editor_open()) {
                        mouse_look_ = true;       // never fly-cam over the code editor
                        SDL_SetWindowRelativeMouseMode(window_, true);
                    }
                } else if (e.button.button == SDL_BUTTON_LEFT) {
                    click_pending_ = true;   // resolved in update_gizmo() after the UI is built
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_RIGHT) {
                    mouse_look_ = false;
                    SDL_SetWindowRelativeMouseMode(window_, false);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (mouse_look_) {                       // wheel = fly-speed while flying
                    if (e.wheel.y > 0)      camera_.move_speed *= 1.15f;
                    else if (e.wheel.y < 0) camera_.move_speed *= 0.87f;
                    if (camera_.move_speed < 0.3f)   camera_.move_speed = 0.3f;
                    if (camera_.move_speed > 300.0f) camera_.move_speed = 300.0f;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_S && (e.key.mod & SDL_KMOD_CTRL) && !editor_.editor_open()) {
                    save_scene(scene_path());
                }
                if (e.key.key == SDLK_ESCAPE) {
                    // Esc no longer quits: leave fly-cam, else clear the selection.
                    if (playing_) toggle_play();   // Esc stops the running game first
                    else if (mouse_look_) { mouse_look_ = false; SDL_SetWindowRelativeMouseMode(window_, false); }
                    else editor_.set_selected(-1);
                }
                // Gizmo mode hotkeys — only while NOT flying (W is WASD movement)
                // and NOT typing into a UI widget (so editing scripts/fields that
                // contain W/E/R doesn't silently switch the gizmo mode).
                if (!mouse_look_ && !nk_item_is_any_active(ui_.ctx())) {
                    if      (e.key.key == SDLK_W) editor_.set_gizmo_mode(0);  // translate
                    else if (e.key.key == SDLK_E) editor_.set_gizmo_mode(1);  // rotate
                    else if (e.key.key == SDLK_R) editor_.set_gizmo_mode(2);  // scale
                }
                break;
            default: break;
        }
    }
    ui_.input_end();
}

// Apply camera look + WASD movement. Skipped while a gizmo handle is being
// dragged (gizmo_active_) so dragging a handle never also flies the camera.
void Engine::apply_camera(float dt) {
    if (gizmo_active_ || !mouse_look_) return;
    camera_.add_look(look_dx_, look_dy_);
    const bool* ks = SDL_GetKeyboardState(nullptr);
    em::vec3 dir{0, 0, 0};
    if (ks[SDL_SCANCODE_W]) dir.z += 1;
    if (ks[SDL_SCANCODE_S]) dir.z -= 1;
    if (ks[SDL_SCANCODE_D]) dir.x += 1;
    if (ks[SDL_SCANCODE_A]) dir.x -= 1;
    if (ks[SDL_SCANCODE_E]) dir.y += 1;
    if (ks[SDL_SCANCODE_Q]) dir.y -= 1;
    float boost = ks[SDL_SCANCODE_LSHIFT] ? 3.0f : 1.0f;
    camera_.move(dir, dt, boost);
}

// Run the transform gizmo for the selected entity. Must be called during UI
// building (after editor_.draw, before ui_.prepare) because it opens its own
// overlay window. Also resolves a pending left-click into a ray-pick selection
// when the gizmo did not consume it and the cursor is not over a panel.
void Engine::update_gizmo() {
    gizmo_active_ = false;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    float mxf = 0, myf = 0;
    Uint32 ms = SDL_GetMouseState(&mxf, &myf);
    const float dens = SDL_GetWindowPixelDensity(window_);
    mxf *= dens; myf *= dens;   // points -> pixels (UI/viewport are in pixels)
    bool ldown = (ms & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0;

    gizmo_.mode = (GizmoMode)editor_.gizmo_mode();
    // Capture UI hover BEFORE gizmo_.manipulate() opens its fullscreen overlay
    // window (which would otherwise make nk_window_is_any_hovered always true).
    // The gizmo opens a fullscreen overlay window, so nk_window_is_any_hovered()
    // always reports "hovered" over the viewport. Use the viewport rect instead:
    // a click inside the 3D viewport is a scene click; outside it is a panel.
    const bool in_viewport = (mxf >= vp_x_ && mxf < vp_x_ + vp_w_ &&
                              myf >= vp_y_ && myf < vp_y_ + vp_h_);

    GizmoResult gz{};
    int sel = editor_.selected();
    if (sel >= 0 && sel < (int)scene_.entities.size()) {
        // Draw the gizmo every frame (so it stays visible while flying); only
        // allow dragging when NOT in fly-cam (right mouse held).
        gizmo_.local_space = editor_.gizmo_local();
        gizmo_.draw_overlay = false;   // visuals come from the 3D gizmo mesh now
        gizmo_.snap_move    = editor_.snap_enabled() ? editor_.snap_grid()  : 0.0f;
        gizmo_.snap_rot_deg = editor_.snap_enabled() ? editor_.snap_angle() : 0.0f;
        gz = gizmo_.manipulate(ui_.ctx(), scene_.entities[sel].transform, camera_,
                               vp_x_, vp_y_, vp_w_, vp_h_, ldown && !mouse_look_, mxf, myf);
    }
    gizmo_active_ = gz.active;
    gizmo_hot_ = gz.axis;
    if (gz.clone) clone_selected();   // Shift+gizmo-drag duplicates; the drag moves the copy

    // If the user dragged the gizmo on a body-backed entity, push the new
    // position into the physics world so a dynamic body doesn't snap back when
    // the drag ends (translate only — set_position takes no rotation).
    if (gz.active && sel >= 0 && sel < (int)scene_.entities.size()) {
        Entity& se = scene_.entities[sel];
        if (se.body.valid()) physics_.set_position(se.body, se.transform.position);
    }

    // Left click that the gizmo didn't grab, and not over an editor panel ->
    // pick the nearest entity under the cursor.
    if (click_pending_ && !mouse_look_ && !gz.hovered && !gz.active && in_viewport) {
        pick_entity(mxf, myf);
    }
    click_pending_ = false;
    left_was_down_ = ldown;
}

// Ray-pick the nearest entity under the cursor (bounding-sphere test) and
// select it. Uses the SAME proj*view the renderer draws with (picking.hpp).
void Engine::pick_entity(float mx, float my) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) return;
    float vw = (vp_w_ > 0) ? vp_w_ : (float)w, vh = (vp_h_ > 0) ? vp_h_ : (float)h;
    pick::Ray r = pick::screen_ray(camera_, vw / vh, mx - vp_x_, my - vp_y_, vw, vh);

    float best = 1e30f;
    int hit = -1;
    for (int i = 0; i < (int)scene_.entities.size(); ++i) {
        const Entity& e = scene_.entities[i];
        if (!e.has_mesh() || e.mesh >= (int)scene_.meshes.size()) continue;
        const GpuMesh& m = scene_.meshes[e.mesh];
        // Transform the ray into the entity's local space and test the actual
        // mesh AABB there: a tight oriented-box test, far more accurate than the
        // old world bounding-sphere (whose radii overlapped badly between parts).
        em::mat4 model = scene_.world_matrix(i);
        em::mat4 inv   = pick::inverse(model);
        em::vec4 lo = inv * em::vec4{r.origin, 1.0f};
        em::vec4 ld = inv * em::vec4{r.dir, 0.0f};
        pick::Ray lr;
        lr.origin = {lo.x, lo.y, lo.z};
        lr.dir    = em::normalize(em::vec3{ld.x, ld.y, ld.z});
        em::vec3 mn = m.bounds_min, mx = m.bounds_max;
        if (mx.x - mn.x < 1e-4f && mx.y - mn.y < 1e-4f && mx.z - mn.z < 1e-4f) {
            mn = {-0.5f, -0.5f, -0.5f}; mx = {0.5f, 0.5f, 0.5f};   // degenerate guard
        }
        float tt;
        if (pick::ray_aabb(lr, mn, mx, tt)) {
            // Convert the local hit back to world space for a distance that is
            // comparable across entities of different scales.
            em::vec3 lhit{lr.origin.x + lr.dir.x * tt,
                          lr.origin.y + lr.dir.y * tt,
                          lr.origin.z + lr.dir.z * tt};
            em::vec4 wh = model * em::vec4{lhit, 1.0f};
            float dx = wh.x - r.origin.x, dy = wh.y - r.origin.y, dz = wh.z - r.origin.z;
            float wd = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (wd < best) { best = wd; hit = i; }
        }
    }
    const bool* ks = SDL_GetKeyboardState(nullptr);
    bool ctrl = ks && (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]);
    if (hit >= 0) { if (ctrl) editor_.toggle_select(hit); else editor_.set_selected(hit); editor_.log("[pick] " + scene_.entities[hit].name); }
    else          { editor_.set_selected(-1); }
}

em::vec3 Engine::spawn_point() const {
    return camera_.position + camera_.forward() * 5.0f;
}

void Engine::spawn_primitive(int kind) {
    em::vec3 p = spawn_point();
    if (kind == 1) {
        spawn_sphere(p);
    } else if (kind == 2) {
        Entity e;
        e.name = "plane_" + std::to_string(scene_.entities.size());
        e.mesh = plane_mesh_; e.material = "grid"; e.transform.position = p;
        e.kind = InstanceKind::Renderable;
        scene_.add_entity(e);
    } else {
        spawn_box(p);
    }
    editor_.set_selected((int)scene_.entities.size() - 1);
}

// Create a typed instance in front of the camera. Empty has no mesh; Renderable
// draws a cube without physics; RigidBody/StaticCollider get a box collider.
void Engine::spawn_instance(int kind) {
    em::vec3 p = spawn_point();
    Entity e;
    e.transform.position = p;
    e.kind = (InstanceKind)kind;
    switch (e.kind) {
        case InstanceKind::Empty:
            e.name = "empty_" + std::to_string(scene_.entities.size());
            e.mesh = -1;
            break;
        case InstanceKind::Renderable:
            e.name = "renderable_" + std::to_string(scene_.entities.size());
            e.mesh = cube_mesh_; e.material = "crate";
            break;
        case InstanceKind::RigidBody:
            e.name = "rigidbody_" + std::to_string(scene_.entities.size());
            e.mesh = cube_mesh_; e.material = "crate";
            e.collider = ColliderType::Box;
            e.body = physics_.add_box({0.5f, 0.5f, 0.5f}, p, true);
            break;
        case InstanceKind::StaticCollider:
            e.name = "static_" + std::to_string(scene_.entities.size());
            e.mesh = cube_mesh_; e.material = "grid";
            e.collider = ColliderType::Box;
            e.body = physics_.add_box({0.5f, 0.5f, 0.5f}, p, false);
            break;
        case InstanceKind::Camera:
            e.name = "camera_" + std::to_string(scene_.entities.size());
            e.mesh = -1; e.fov = 60.0f;
            break;
    }
    scene_.add_entity(e);
    physics_.optimize();
    editor_.set_selected((int)scene_.entities.size() - 1);
    editor_.log("[scene] created " + e.name);
}

std::string Engine::material_for_primitive(const Primitive& prim) {
    // Build (or reuse) a material from the GLB primitive's base colour so models
    // render in their authored colours instead of the grey "default".
    em::vec4 c = prim.base_color;
    auto cl = [](float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
    std::string k;
    if (!prim.tex_rgba.empty()) {
        k = "glbtex_" + prim.tex_key;
    } else {
        char key[48];
        std::snprintf(key, sizeof key, "glbmat_%02x%02x%02x%02x",
                      (int)(cl(c.x) * 255), (int)(cl(c.y) * 255),
                      (int)(cl(c.z) * 255), (int)(cl(c.w) * 255));
        k = key;
    }
    if (!materials_.count(k)) {
        Material m;
        m.name = k;
        m.color_tint = c;
        m.roughness = 0.7f;
        if (!prim.tex_rgba.empty()) {
            m.tex = gpu::create_texture_rgba(gpu_, prim.tex_rgba.data(),
                                             (uint32_t)prim.tex_w, (uint32_t)prim.tex_h, k.c_str());
            m.use_texture = (m.tex != nullptr);
            if (!m.tex) m.tex = renderer_.white();
        } else {
            material::resolve_gpu(gpu_, m, renderer_.white());
        }
        materials_[k] = m;
    }
    return k;
}

void Engine::instantiate_model(const std::string& path) {
    if (!std::filesystem::exists(path)) { editor_.log("[glb] missing " + path); return; }
    Model mdl;
    if (!model::load_glb(path, mdl)) { editor_.log("[glb] failed " + path); return; }
    em::vec3 base = spawn_point();
    std::string nm = std::filesystem::path(path).stem().string();
    Entity grp; grp.name = nm; grp.kind = InstanceKind::Empty; grp.mesh = -1;
    grp.transform.position = base;
    int pi = (int)scene_.entities.size(); scene_.add_entity(grp);
    int gi = 0;
    for (const Primitive& prim : mdl.primitives) {
        int mesh = add_mesh(prim.mesh);
        glb_mesh_src_[mesh] = "glb:" + path + ":" + std::to_string(gi);
        Entity e;
        e.name = nm + "_part" + std::to_string(gi++);
        e.mesh = mesh;
        e.material = material_for_primitive(prim);
        e.parent = pi;
        scene_.add_entity(e);
    }
    editor_.set_selected(pi);
    editor_.log("[glb] instantiated " + nm);
}

void Engine::assign_material(const std::string& name) {
    int sel = editor_.selected();
    if (sel < 0 || sel >= (int)scene_.entities.size()) { editor_.log("[mat] select an entity first"); return; }
    if (!materials_.count(name)) {
        // newly-added .tgmat not loaded yet — pull it in on demand
        for (const AssetEntry& a : project_.materials())
            if (a.stem == name) {
                Material m;
                if (material::load_tgmat(a.path, m)) {
                    m.name = name;
                    material::resolve_gpu(gpu_, m, renderer_.white());
                    materials_[name] = m;
                }
                break;
            }
    }
    if (!materials_.count(name)) { editor_.log("[mat] unknown material " + name); return; }
    scene_.entities[sel].material = name;
    editor_.log("[mat] " + scene_.entities[sel].name + " -> " + name);
}

// Assign (or clear, when stem is empty) a gameplay script on the selected entity.
void Engine::assign_script(const std::string& stem) {
    int sel = editor_.selected();
    if (sel < 0 || sel >= (int)scene_.entities.size()) { editor_.log("[script] select an entity first"); return; }
    scene_.entities[sel].script = stem;
    editor_.log(stem.empty() ? "[script] cleared on " + scene_.entities[sel].name
                             : "[script] " + scene_.entities[sel].name + " -> " + stem);
}

// Build a physics collider for the selected entity from its mesh bounds (or, for
// Convex, from its retained CPU vertices). dynamic-ness follows the InstanceKind.
void Engine::generate_collider(int collider_type) {
    int sel = editor_.selected();
    if (sel < 0 || sel >= (int)scene_.entities.size()) { editor_.log("[phys] select an entity first"); return; }
    Entity& e = scene_.entities[sel];
    ColliderType ct = (ColliderType)collider_type;

    bool dyn = (e.kind == InstanceKind::RigidBody);

    // Drop any existing body first so we never leak / double-integrate.
    if (e.body.valid()) { physics_.remove_body(e.body); e.body = PhysicsBody{}; }
    if (ct == ColliderType::None) { e.collider = ColliderType::None; editor_.log("[phys] collider removed"); return; }

    em::vec3 s = e.transform.scale;

    if (ct == ColliderType::Convex) {
        if (e.mesh < 0 || e.mesh >= (int)mesh_cpu_.size() || mesh_cpu_[e.mesh].vertices.empty()) {
            editor_.log("[phys] convex needs a mesh with CPU vertices; falling back to box");
            ct = ColliderType::Box;
        } else {
            const MeshData& md = mesh_cpu_[e.mesh];
            std::vector<em::vec3> pts; pts.reserve(md.vertices.size());
            for (const Vertex& v : md.vertices)
                pts.push_back({v.pos.x * s.x, v.pos.y * s.y, v.pos.z * s.z});
            e.body = physics_.add_convex(pts, e.transform.position, dyn);
            if (!e.body.valid()) {
                editor_.log("[phys] convex hull build failed; falling back to box");
                ct = ColliderType::Box;
            }
        }
    }

    if (ct != ColliderType::Convex) {
        // Derive dimensions from the mesh AABB (scaled). Entities with no mesh
        // get a unit box so Empty/script nodes can still have a trigger volume.
        em::vec3 half{0.5f * s.x, 0.5f * s.y, 0.5f * s.z};
        em::vec3 pos = e.transform.position;
        if (e.mesh >= 0 && e.mesh < (int)scene_.meshes.size()) {
            const GpuMesh& gm = scene_.meshes[e.mesh];
            em::vec3 c   = (gm.bounds_min + gm.bounds_max) * 0.5f;
            em::vec3 ext = (gm.bounds_max - gm.bounds_min) * 0.5f;
            half = {ext.x * s.x, ext.y * s.y, ext.z * s.z};
            pos  = e.transform.position + em::vec3{c.x * s.x, c.y * s.y, c.z * s.z};
        }
        switch (ct) {
            case ColliderType::Sphere: {
                float r = std::max({half.x, half.y, half.z});
                e.body = physics_.add_sphere(r, pos, dyn);
                break;
            }
            case ColliderType::Capsule: {
                float r  = std::max(half.x, half.z);
                float hh = std::max(0.0f, half.y - r);
                e.body = physics_.add_capsule(r, hh, pos, dyn);
                break;
            }
            case ColliderType::Box:
            default:
                e.body = physics_.add_box(half, pos, dyn);
                break;
        }
    }

    e.collider = ct;
    physics_.optimize();
    editor_.log("[phys] " + e.name + " collider -> " +
                (ct == ColliderType::Box ? "Box" : ct == ColliderType::Sphere ? "Sphere" :
                 ct == ColliderType::Capsule ? "Capsule" : "Convex") +
                (dyn ? " (dynamic)" : " (static)"));
}

// Change the selected entity's InstanceKind, (re)generating or removing its
// physics body to match the new role.
void Engine::set_kind(int kind) {
    int sel = editor_.selected();
    if (sel < 0 || sel >= (int)scene_.entities.size()) return;
    Entity& e = scene_.entities[sel];
    e.kind = (InstanceKind)kind;

    if (e.kind == InstanceKind::Empty || e.kind == InstanceKind::Renderable) {
        // No physics in these roles: drop any existing body.
        if (e.body.valid()) { physics_.remove_body(e.body); e.body = PhysicsBody{}; }
        e.collider = ColliderType::None;
    } else {
        // RigidBody / StaticCollider: ensure a collider exists (default Box) and
        // rebuild it with the correct dynamic flag.
        ColliderType ct = e.collider == ColliderType::None ? ColliderType::Box : e.collider;
        generate_collider((int)ct);   // also calls optimize()
    }
    editor_.log("[scene] " + e.name + " kind changed");
}

// Remove an entity (and its physics body) by index, fixing up the selection.
void Engine::delete_entity(int index) {
    if (index < 0 || index >= (int)scene_.entities.size()) return;
    Entity& e = scene_.entities[index];
    if (e.body.valid()) { physics_.remove_body(e.body); e.body = PhysicsBody{}; }
    editor_.log("[scene] deleted " + e.name);
    scene_.entities.erase(scene_.entities.begin() + index);
    // NOTE: scene_.meshes / mesh_cpu_ are intentionally NOT compacted so other
    // entities' mesh indices stay valid (the GPU buffer is freed at shutdown).
    editor_.set_selected(-1);
}

void Engine::delete_selected() {
    delete_entity(editor_.selected());
}

void Engine::clone_selected() {
    int sel = editor_.selected();
    if (sel < 0 || sel >= (int)scene_.entities.size()) return;
    Entity copy = scene_.entities[sel];          // shares the mesh index; body re-made below
    copy.name += "_copy";
    ColliderType ct = copy.collider;
    copy.body = PhysicsBody{};
    copy.collider = ColliderType::None;
    scene_.entities.push_back(copy);
    int ni = (int)scene_.entities.size() - 1;
    editor_.set_selected(ni);
    if (ct != ColliderType::None) generate_collider((int)ct);   // rebuild the collider/body
    editor_.set_selected(ni);
    editor_.log("[scene] cloned -> " + scene_.entities[ni].name);
}

void Engine::toggle_play() {
    playing_ = !playing_;
    if (playing_) {
        play_snapshot_.clear();
        play_snapshot_.reserve(scene_.entities.size());
        for (Entity& e : scene_.entities) {
            play_snapshot_.push_back(e.transform);
            if (e.body.valid()) physics_.set_position(e.body, e.transform.position);
        }
        physics_.optimize();
        editor_.log("[play] simulation started");
    } else {
        for (size_t i = 0; i < scene_.entities.size() && i < play_snapshot_.size(); ++i) {
            scene_.entities[i].transform = play_snapshot_[i];
            if (scene_.entities[i].body.valid())
                physics_.set_position(scene_.entities[i].body, play_snapshot_[i].position);
        }
        editor_.log("[play] stopped - scene restored");
    }
    editor_.set_playing(playing_);
}

void Engine::process_editor_actions() {
    EditorAction a;
    while (editor_.poll_action(a)) {
        switch (a.type) {
            case EditorAction::SpawnPrimitive:   spawn_primitive(a.i);   break;
            case EditorAction::SpawnInstance:    spawn_instance(a.i);    break;
            case EditorAction::InstantiateModel: instantiate_model(a.s); break;
            case EditorAction::AssignMaterial:   assign_material(a.s);   break;
            case EditorAction::AssignScript:     assign_script(a.s);     break;
            case EditorAction::SetKind:          set_kind(a.i);          break;
            case EditorAction::GenerateCollider: generate_collider(a.i); break;
            case EditorAction::CloneSelected:    clone_selected();       break;
            case EditorAction::DeleteSelected:   delete_selected();      break;
            case EditorAction::ResetScene:       reset_scene();          break;
            case EditorAction::SaveScene:        save_scene(scene_path()); break;
            case EditorAction::LoadScene:        load_scene(scene_path()); break;
            case EditorAction::TogglePlay:       toggle_play();          break;
        }
    }
}

void Engine::sync_physics() {
    int sel = editor_.selected();
    for (int i = 0; i < (int)scene_.entities.size(); ++i) {
        Entity& e = scene_.entities[i];
        // Only dynamic bodies are driven by the simulation; static colliders keep
        // their authored transform (so the gizmo can move them). Also skip the
        // selected entity while a gizmo handle is being dragged so the drag wins.
        if (!e.body.valid() || !e.is_dynamic()) continue;
        if (gizmo_active_ && i == sel) continue;
        em::vec3 p; em::quat r;
        physics_.get_transform(e.body, p, r);
        e.transform.position = p;
        e.transform.rotation = r;
    }
}

// ----------------------------------------------------------------------------- loop
void Engine::run() {
    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    float fps_avg = 60.0f;

    while (running_) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / freq);
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        fps_avg = fps_avg * 0.9f + (dt > 0 ? 1.0f / dt : 0.0f) * 0.1f;

        handle_input(dt);                 // poll events (no camera movement yet)
        if (playing_) { physics_.update(dt); sync_physics(); }

        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window_, &pw, &ph);
        // UI building phase: editor panels first (sets selection), then the gizmo
        // overlay (which can veto the camera), then apply deferred camera input.
        editor_.draw(ui_.ctx(), pw, ph, scene_, camera_, physics_, project_, fps_avg, icons_);
        editor_.viewport_rect(vp_x_, vp_y_, vp_w_, vp_h_);
        update_gizmo();
        apply_camera(dt);
        process_editor_actions();

        renderer_.render(scene_, camera_, materials_, ui_, vp_x_, vp_y_, vp_w_, vp_h_,
                     editor_.selected(), editor_.show_gizmos() ? editor_.gizmo_mode() : -1,
                     editor_.gizmo_local(), gizmo_hot_, editor_.show_colliders(), editor_.selection());
    }
}

// ----------------------------------------------------------------------------- shutdown
void Engine::shutdown() {
    // Let every in-flight command buffer finish so the swapchain and per-frame
    // GPU resources are released exactly once.
    if (gpu_) SDL_WaitForGPUIdle(gpu_);

    icons_.shutdown();   // release icon GPU textures before destroying the device
    for (GpuMesh& m : scene_.meshes) model::destroy(gpu_, m);
    physics_.shutdown();
    editor_.shutdown();
    ui_.shutdown();
    renderer_.shutdown();

    if (gpu_ && window_ && window_claimed_) SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    if (gpu_)    SDL_DestroyGPUDevice(gpu_);
    if (window_) SDL_DestroyWindow(window_);

    // NOTE: we intentionally do NOT call SDL_Quit(). On several SDL3 + Mesa
    // Vulkan stacks SDL_Quit() double-frees a driver allocation during global
    // teardown (confirmed with AddressSanitizer). The GPU device and window are
    // already released above, so the process exits cleanly and the OS reclaims
    // the rest. Set ENGINE_GPU_DEBUG=1 to re-enable Vulkan validation layers.
}

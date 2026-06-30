#pragma once
// The editor: menu bar + asset browser + outliner + inspector + console +
// an in-engine C++ script editor (templates, colored + EDITABLE source widget,
// compile). Panels live inside a DockSpace (Left/Right/Bottom zones + an empty
// Center for the 3D viewport). The editor reads/writes scene data directly and
// raises queued actions that the engine (which owns the GPU/scene/physics)
// performs.
#include <string>

struct nk_context;
struct Scene;
struct Camera;
class  PhysicsWorld;
class  Project;
class  IconSet;   // SVG icon textures, owned by the Engine and passed in by ref
struct nk_user_font;

// An action the editor asks the engine to perform (engine owns GPU + scene).
struct EditorAction {
    enum Type {
        InstantiateModel,   // s = .glb path
        SpawnPrimitive,     // i = 0 cube / 1 sphere / 2 plane
        SpawnInstance,      // i = InstanceKind (0 Empty,1 Renderable,2 RigidBody,3 StaticCollider)
        AssignMaterial,     // s = material name -> selected entity
        AssignScript,       // s = script stem ("" clears) -> selected entity
        SetKind,            // i = InstanceKind -> selected entity (regenerates body)
        GenerateCollider,   // i = ColliderType (1 Box,2 Sphere,3 Capsule,4 Convex)
        CloneSelected,
        DeleteSelected,
        ResetScene,
        SaveScene,
        LoadScene,
        TogglePlay,         // start/stop in-editor simulation
    };
    Type        type;
    std::string s;
    int         i = 0;
};

class Editor {
public:
    void init();
    void shutdown();

    // Build every panel for this frame. `project` provides the asset lists and
    // `icons` provides the UI glyphs (passed by const ref; the editor only reads
    // them). The DockSpace owns panel layout; the top toolbar stays free-floating.
    void draw(nk_context* ctx, int screen_w, int screen_h,
              Scene& scene, Camera& cam, PhysicsWorld& physics,
              Project& project, float fps, const IconSet& icons);

    void log(const std::string& line);

    // Engine drains queued actions after draw(). Returns false when empty.
    bool poll_action(EditorAction& out);

    int selected() const;
    void set_selected(int i);
    void toggle_select(int i);                 // Ctrl+click add/remove
    bool is_selected(int i) const;             // in the multi-selection set
    const std::vector<int>& selection() const; // all highlighted entities

    // The engine queries this to drive the transform gizmo's mode (W/E/R keys
    // set it through here so the editor toolbar and keyboard agree). 0 = Translate,
    // 1 = Rotate, 2 = Scale.
    int  gizmo_mode() const;
    bool gizmo_local() const;
    bool editor_open() const;   // true when the code editor covers the viewport
    bool show_gizmos() const;
    bool show_colliders() const;
    bool  snap_enabled() const;
    float snap_grid() const;
    float snap_angle() const;
    void set_gizmo_mode(int m);

    // Reflect the engine's play/edit state on the toolbar Play/Stop button.
    void set_playing(bool playing);

    // The 3D viewport (dock Center) rect, valid after draw(). The engine uses it
    // to place the GPU viewport / picking so the scene sits in the middle.
    void viewport_rect(float& x, float& y, float& w, float& h) const;

    // Monospace font (JetBrains Mono) the code editor renders with. Set once by
    // the engine after the Nuklear backend has baked its fonts.
    void set_mono_font(const struct nk_user_font* f);

    struct Impl;   // opaque; defined in editor.cpp (named by internal helpers)
private:
    Impl* p = nullptr;
};

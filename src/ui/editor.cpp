#include "pch.hpp"
#include <cmath>
#include "ui/editor.hpp"
#include "ui/nuklear_config.hpp"
#include "ui/icons.hpp"
#include "ui/dock.hpp"
#include "ui/code_editor.hpp"
#include "scene/scene.hpp"
#include "camera/camera.hpp"
#include "physics/physics.hpp"
#include "project/project.hpp"

#include <atomic>
#include <cstdio>
#include <algorithm>
#include <unordered_map>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ---- thread-safe console ----------------------------------------------------
struct Console {
    std::mutex mtx;
    std::vector<std::string> lines;
    void add(std::string s) {
        std::lock_guard<std::mutex> lk(mtx);
        lines.push_back(std::move(s));
        if (lines.size() > 800) lines.erase(lines.begin(), lines.begin() + (lines.size() - 800));
    }
    std::vector<std::string> snapshot() { std::lock_guard<std::mutex> lk(mtx); return lines; }
    void clear() { std::lock_guard<std::mutex> lk(mtx); lines.clear(); }
};

// ---- asynchronous command runner (engine build + script compile) -----------
struct BuildJob {
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::atomic<int>  last_rc{0};
    ~BuildJob() { if (th.joinable()) th.join(); }
    void start(const std::string& cmd, Console* con) {
        if (running) return;
        if (th.joinable()) th.join();
        running = true; finished = false;
        con->add("[build] $ " + cmd);
        th = std::thread([cmd, con, this] {
            FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
            if (!pipe) { con->add("[build] failed to launch build"); running = false; finished = true; last_rc = -1; return; }
            char buf[1024];
            while (fgets(buf, sizeof buf, pipe)) {
                std::string line(buf);
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                con->add(line);
            }
            int rc = pclose(pipe);
            last_rc = rc;
            con->add(rc == 0 ? "[build] SUCCESS" : "[build] FAILED (rc=" + std::to_string(rc) + ")");
            finished = true; running = false;
        });
    }
};

void launch_detached(const std::string& path, Console* con) {
    con->add("[run] launching " + path);
    int rc = std::system(("setsid " + path + " >/dev/null 2>&1 &").c_str());
    (void)rc;
}

std::string shq(const std::string& s) { return "'" + s + "'"; } // shell-quote

// ---- script templates -------------------------------------------------------
const char* kTemplates[] = {
    "#include \"engine_api.hpp\"\nusing namespace game;\n\nvoid on_update(Entity& self, float dt) {\n\n}\n",
    "#include \"engine_api.hpp\"\nusing namespace game;\n\nvoid on_start(Entity& self) {\n    log(\"started\\n\");\n}\n\nvoid on_update(Entity& self, float dt) {\n    Vec3 p = get_position(self);\n    p.y += dt;\n    set_position(self, p);\n}\n",
    "#include \"engine_api.hpp\"\nusing namespace game;\n\nstruct Health {\n    float hp = 100.0f;\n    void damage(float d) { hp -= d; }\n};\n",
};
// nk_combo_string splits on EMBEDDED NUL bytes (not ';'), so the items must be
// a single buffer with '\0' separators and a final terminating '\0'.
const char  kTemplateNames[] = "Empty\0Spinner\0Component\0";

// Icon stem for an entity's instance kind (used in the outliner / inspector).
const char* kind_icon(InstanceKind k) {
    switch (k) {
        case InstanceKind::Empty:          return "empty";
        case InstanceKind::Renderable:     return "mesh";
        case InstanceKind::RigidBody:      return "rigidbody";
        case InstanceKind::StaticCollider: return "static";
        case InstanceKind::Camera:         return "camera";
    }
    return "empty";
}
const char* collider_name(ColliderType c) {
    switch (c) {
        case ColliderType::None:    return "None";
        case ColliderType::Box:     return "Box";
        case ColliderType::Sphere:  return "Sphere";
        case ColliderType::Capsule: return "Capsule";
        case ColliderType::Convex:  return "Convex";
    }
    return "None";
}
// Icon stem for a Project::ScriptStatus badge.
const char* status_icon(Project::ScriptStatus s) {
    switch (s) {
        case Project::ScriptStatus::Compiled: return "script_ok";
        case Project::ScriptStatus::Outdated: return "script_outdated";
        case Project::ScriptStatus::None:     return "script_none";
    }
    return "script_none";
}

} // namespace

// =============================================================================
struct Editor::Impl {
    Console console;
    BuildJob build;
    Project* project = nullptr;

    DockSpace dock;

    int   selected = -1;                 // primary (inspector/gizmo)
    std::vector<int> selection;          // full highlighted set (multi-select)
    char  build_cmd[256] = "go-task build";
    char  run_path[256]  = "./build/main-dev";
    bool  run_after_build = false;
    int   asset_tab = 0;
    int   gizmo_mode = 0;   // 0 Translate, 1 Rotate, 2 Scale (mirrors engine Gizmo)
    bool  gizmo_local = false;   // false=global axes, true=object-local
    bool  show_gizmos = true;
    bool  show_colliders = true;
    bool  snap_on = false;
    float snap_grid = 0.5f;
    float snap_angle = 15.0f;
    bool  playing    = false;  // mirror of the engine play/edit state
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;   // dock Center (viewport) rect

    bool  show_editor = false;
    char  script_name[128] = "NewScript.cpp";
    std::vector<char> script_buf;  // canonical script text buffer
    int   template_sel = 1;

    // Editable + syntax-coloured source widget. `script_buf` stays the canonical
    // buffer that Save / Insert / snippet buttons write to; `code` mirrors it.
    CodeEditor code;
    float insp_euler[3] = {0.0f, 0.0f, 0.0f};   // inspector rotation (deg)
    std::unordered_map<std::string, bool> sections;   // custom collapse headers
    std::string edit_id;            // which num_field is being typed into
    char        edit_buf[40] = {0};
    int   insp_euler_for = -2;
    bool save_key_down = false;   // Ctrl+S edge detection
    bool       code_loaded = false;
    const nk_user_font* mono = nullptr;   // code editor font (JetBrains Mono)

    std::vector<EditorAction> actions;

    Impl() {
        script_buf.assign(1 << 16, 0);
        std::strcpy(script_buf.data(), kTemplates[1]);
        dock.set_top_strip(84.0f);   // matches the toolbar window height below
    }
    void queue(EditorAction a) { actions.push_back(std::move(a)); }
};

// ----------------------------------------------------------------------------- API
void Editor::init() {
    p = new Impl();
    log("Editor ready. Right-drag to look, WASD to fly. W/E/R = move/rotate/scale.");
}
void Editor::shutdown() { delete p; p = nullptr; }
void Editor::log(const std::string& s) { if (p) p->console.add(s); }
int  Editor::selected() const { return p ? p->selected : -1; }
void Editor::set_selected(int i) {
    if (!p) return;
    p->selected = i;
    p->selection.clear();
    if (i >= 0) p->selection.push_back(i);
}
void Editor::toggle_select(int i) {
    if (!p || i < 0) return;
    auto& sel = p->selection;
    auto it = std::find(sel.begin(), sel.end(), i);
    if (it != sel.end()) { sel.erase(it); p->selected = sel.empty() ? -1 : sel.back(); }
    else { sel.push_back(i); p->selected = i; }
}
bool Editor::is_selected(int i) const {
    return p && std::find(p->selection.begin(), p->selection.end(), i) != p->selection.end();
}
static const std::vector<int> kEmptySel;
const std::vector<int>& Editor::selection() const { return p ? p->selection : kEmptySel; }
void Editor::set_mono_font(const struct nk_user_font* f) { if (p) p->mono = f; }
int  Editor::gizmo_mode() const { return p ? p->gizmo_mode : 0; }
void Editor::set_gizmo_mode(int m) { if (p) p->gizmo_mode = m; }
bool Editor::gizmo_local() const { return p ? p->gizmo_local : false; }
bool Editor::editor_open() const { return p ? p->show_editor : false; }
bool Editor::show_gizmos() const { return p ? p->show_gizmos : true; }
bool Editor::show_colliders() const { return p ? p->show_colliders : true; }
bool  Editor::snap_enabled() const { return p ? p->snap_on : false; }
float Editor::snap_grid() const { return p ? p->snap_grid : 0.5f; }
float Editor::snap_angle() const { return p ? p->snap_angle : 15.0f; }
void Editor::set_playing(bool pl) { if (p) p->playing = pl; }
void Editor::viewport_rect(float& x, float& y, float& w, float& h) const {
    if (p) { x = p->vp_x; y = p->vp_y; w = p->vp_w; h = p->vp_h; } else { x = y = w = h = 0; }
}
bool Editor::poll_action(EditorAction& out) {
    if (!p || p->actions.empty()) return false;
    out = p->actions.front();
    p->actions.erase(p->actions.begin());
    return true;
}

// ----------------------------------------------------------------------------- helpers
static void style_green(nk_context* ctx, nk_style_button& saved) {
    saved = ctx->style.button;
    ctx->style.button.normal = nk_style_item_color(nk_rgb(55, 135, 70));
    ctx->style.button.hover  = nk_style_item_color(nk_rgb(80, 175, 95));
    ctx->style.button.active = nk_style_item_color(nk_rgb(40, 110, 55));
    ctx->style.button.text_normal = nk_rgb(255, 255, 255);
}

// Push `script_buf` into the code editor next frame (after an external mutation).
static void resync_code(Editor::Impl* d) { d->code_loaded = false; }

static void load_into_editor(Editor::Impl* d, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { d->console.add("[edit] cannot open " + path); return; }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    if (content.size() >= d->script_buf.size()) content.resize(d->script_buf.size() - 1);
    std::memcpy(d->script_buf.data(), content.data(), content.size());
    d->script_buf[content.size()] = 0;
    std::string base = std::filesystem::path(path).filename().string();
    std::strncpy(d->script_name, base.c_str(), sizeof(d->script_name) - 1);
    d->show_editor = true;
    resync_code(d);
    d->console.add("[edit] opened " + path);
}

// A small icon + label row used pervasively in the panels.
static void icon_label(nk_context* ctx, const IconSet& icons, const char* stem,
                       const char* text, float label_w) {
    const float S = ctx->style.font->height / 18.0f;
    nk_layout_row_template_begin(ctx, ctx->style.font->height + 6.0f);
    nk_layout_row_template_push_static(ctx, ctx->style.font->height + 6.0f);
    if (label_w > 0) nk_layout_row_template_push_static(ctx, label_w * S);
    else             nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_image(ctx, icons.get(stem));
    nk_label(ctx, text, NK_TEXT_LEFT);
}

// ----------------------------------------------------------------------------- panels
// Each panel below contains ONLY the body (no nk_begin/nk_end); the DockSpace
// owns the windows. They are invoked between dock.begin_panel()/end_panel().

// Custom folder header: indent + small triangle + name; persistent collapse.
static bool folder_header(nk_context* ctx, Editor::Impl* d, const std::string& name,
                          const std::string& key, int depth) {
    auto it = d->sections.find(key);
    bool open = (it != d->sections.end()) && it->second;   // folders default CLOSED
    const float fh = ctx->style.font->height;
    nk_layout_row_dynamic(ctx, fh + 6.0f, 1);
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) { d->sections[key] = open; return open; }
    struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
    const struct nk_input* in = &ctx->input;
    bool hover = nk_input_is_mouse_hovering_rect(in, b);
    if (hover && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) open = !open;
    d->sections[key] = open;
    if (nk_contextual_begin(ctx, 0, nk_vec2(fh * 9.0f, fh * 5.0f), b)) {
        nk_layout_row_dynamic(ctx, fh + 8.0f, 1);
        if (nk_contextual_item_label(ctx, "New Folder", NK_TEXT_LEFT)) {
            std::error_code mec; std::filesystem::create_directory(key + "/New Folder", mec);
        }
        nk_contextual_end(ctx);
    }
    float ind = (float)depth * fh * 0.85f;
    nk_color bg = hover ? nk_rgb(62, 64, 70) : nk_rgb(43, 45, 50);
    if (hover) nk_fill_rect(canvas, b, 2.0f, bg);
    float cx = b.x + ind + fh * 0.5f, cy = b.y + b.h * 0.5f, t = fh * 0.2f;
    nk_color ac = nk_rgb(205, 205, 210);
    if (open) nk_fill_triangle(canvas, cx - t, cy - t * 0.5f, cx + t, cy - t * 0.5f, cx, cy + t * 0.8f, ac);
    else      nk_fill_triangle(canvas, cx - t * 0.5f, cy - t, cx - t * 0.5f, cy + t, cx + t * 0.8f, cy, ac);
    struct nk_rect tr = nk_rect(b.x + ind + fh, b.y, b.w - ind - fh, b.h);
    nk_draw_text(canvas, tr, name.c_str(), (int)name.size(), ctx->style.font, bg, nk_rgb(230, 230, 235));
    return open;
}

// Recursive custom file tree (real tree: small arrows + indentation).
static void asset_tree(nk_context* ctx, Editor::Impl* d, Project& proj,
                       const IconSet& icons, const std::string& dir, int depth) {
    namespace fs = std::filesystem;
    const float S = ctx->style.font->height / 18.0f;
    const float fh = ctx->style.font->height;
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) dirs.push_back(e); else files.push_back(e);
    }
    auto byname = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string(); };
    std::sort(dirs.begin(), dirs.end(), byname);
    std::sort(files.begin(), files.end(), byname);
    for (const auto& e : dirs) {
        std::string nm = e.path().filename().string();
        if (nm.empty() || nm[0] == '.' || nm == "build") continue;
        std::string p = e.path().string();
        if (folder_header(ctx, d, nm, p, depth))
            asset_tree(ctx, d, proj, icons, p, depth + 1);
    }
    for (const auto& e : files) {
        std::string nm = e.path().filename().string();
        std::string ext = e.path().extension().string();
        std::string path = e.path().string();
        const char* ic = (ext == ".glb") ? "mesh" : (ext == ".tgmat") ? "material" : "script_none";
        nk_layout_row_template_begin(ctx, fh + 6.0f);
        nk_layout_row_template_push_static(ctx, (float)depth * fh * 0.85f + 3.0f);
        nk_layout_row_template_push_static(ctx, fh + 4.0f);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 66.0f * S);
        nk_layout_row_template_end(ctx);
        nk_spacing(ctx, 1);
        nk_image(ctx, icons.get(ic));
        nk_label(ctx, nm.c_str(), NK_TEXT_LEFT);
        if (ext == ".glb") { if (nk_button_label(ctx, "Add")) d->queue({EditorAction::InstantiateModel, path, 0}); }
        else if (ext == ".tgmat") { if (nk_button_label(ctx, "Assign")) d->queue({EditorAction::AssignMaterial, e.path().stem().string(), 0}); }
        else if (ext == ".cpp" && nm != "engine_api.hpp") { if (nk_button_label(ctx, "Edit")) load_into_editor(d, path); }
        else nk_label(ctx, "", NK_TEXT_LEFT);
    }
}

static void body_assets(nk_context* ctx, Editor::Impl* d, Project& proj, const IconSet& icons) {
    const float S = ctx->style.font->height / 18.0f;
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "Project: %s", proj.name().c_str());

    nk_layout_row_dynamic(ctx, ctx->style.font->height + 10.0f, 1);
    if (nk_button_label(ctx, "Refresh")) proj.refresh();
    nk_layout_row_dynamic(ctx, 300.0f * S, 1);
    if (nk_group_begin(ctx, "asset_tree", NK_WINDOW_BORDER)) {
        asset_tree(ctx, d, proj, icons, proj.root(), 0);
        nk_group_end(ctx);
    }

    nk_layout_row_dynamic(ctx, ctx->style.font->height + 12.0f, 2);
    if (nk_button_label(ctx, "New Script")) {
        d->show_editor = true; std::strcpy(d->script_name, "NewScript.cpp");
    }
    if (nk_button_label(ctx, "Open Folder"))
        proj.open_in_file_manager();
}

// One outliner row + its children, recursively (tree + collapse + multi-select).
static void outliner_node(nk_context* ctx, Editor::Impl* d, Scene& scene,
                          const IconSet& icons, int i, int depth) {
    if (depth > 32) return;
    Entity& e = scene.entities[i];
    const float S = ctx->style.font->height / 18.0f;
    const float fh = ctx->style.font->height;
    bool has_children = false;
    for (int j = 0; j < (int)scene.entities.size(); ++j)
        if (scene.entities[j].parent == i) { has_children = true; break; }
    std::string ckey = "outl" + std::to_string(i);
    bool collapsed = has_children && d->sections.count(ckey) && d->sections[ckey];
    bool active = std::find(d->selection.begin(), d->selection.end(), i) != d->selection.end();

    nk_layout_row_template_begin(ctx, fh + 6.0f);
    nk_layout_row_template_push_static(ctx, (float)depth * fh * 0.8f + 3.0f);   // indent
    nk_layout_row_template_push_static(ctx, fh * 0.8f);                          // collapse arrow
    nk_layout_row_template_push_static(ctx, fh + 6.0f);                          // icon
    nk_layout_row_template_push_dynamic(ctx);                                    // name
    if (e.has_script()) nk_layout_row_template_push_static(ctx, fh + 6.0f);
    nk_layout_row_template_end(ctx);
    nk_spacing(ctx, 1);
    // collapse triangle (only for parents)
    {
        struct nk_rect ab;
        if (nk_widget(&ab, ctx) != NK_WIDGET_INVALID && has_children) {
            struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
            const struct nk_input* in = &ctx->input;
            if (nk_input_is_mouse_hovering_rect(in, ab) && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT))
                d->sections[ckey] = !collapsed;
            float cx = ab.x + ab.w * 0.5f, cy = ab.y + ab.h * 0.5f, t = fh * 0.18f;
            nk_color ac = nk_rgb(190, 190, 195);
            if (collapsed) nk_fill_triangle(canvas, cx - t * 0.5f, cy - t, cx - t * 0.5f, cy + t, cx + t * 0.7f, cy, ac);
            else           nk_fill_triangle(canvas, cx - t, cy - t * 0.5f, cx + t, cy - t * 0.5f, cx, cy + t * 0.7f, ac);
        }
    }
    nk_image(ctx, icons.get(kind_icon(e.kind)));
    struct nk_rect rb = nk_widget_bounds(ctx);
    if (nk_select_label(ctx, e.name.c_str(), NK_TEXT_LEFT, active)) {
        if (nk_input_is_key_down(&ctx->input, NK_KEY_CTRL)) {        // Ctrl+click toggles
            auto& sel = d->selection;
            auto it = std::find(sel.begin(), sel.end(), i);
            if (it != sel.end()) { sel.erase(it); d->selected = sel.empty() ? -1 : sel.back(); }
            else { sel.push_back(i); d->selected = i; }
        } else { d->selected = i; d->selection.clear(); d->selection.push_back(i); }
    }
    if (e.has_script()) nk_image(ctx, icons.get("script_ok"));
    if (nk_contextual_begin(ctx, 0, nk_vec2(220.0f * S, 280.0f * S), rb)) {
        nk_layout_row_dynamic(ctx, fh + 8.0f, 1);
        if (nk_contextual_item_label(ctx, "Clone", NK_TEXT_LEFT))  { d->selected = i; d->selection = {i}; d->queue({EditorAction::CloneSelected, "", 0}); }
        if (nk_contextual_item_label(ctx, "Delete", NK_TEXT_LEFT)) { d->selected = i; d->selection = {i}; d->queue({EditorAction::DeleteSelected, "", 0}); }
        if (nk_contextual_item_label(ctx, "Parent to selected", NK_TEXT_LEFT)) {
            if (d->selected >= 0 && d->selected != i && scene.entities[d->selected].parent != i) e.parent = d->selected;
        }
        if (nk_contextual_item_label(ctx, "Unparent", NK_TEXT_LEFT)) e.parent = -1;
        nk_contextual_end(ctx);
    }
    if (!collapsed)
        for (int j = 0; j < (int)scene.entities.size(); ++j)
            if (scene.entities[j].parent == i) outliner_node(ctx, d, scene, icons, j, depth + 1);
}

static void body_outliner(nk_context* ctx, Editor::Impl* d, Scene& scene, const IconSet& icons) {
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_labelf(ctx, NK_TEXT_LEFT, "%zu entities", scene.entities.size());
    for (int i = 0; i < (int)scene.entities.size(); ++i)
        if (scene.entities[i].parent < 0 || scene.entities[i].parent >= (int)scene.entities.size())
            outliner_node(ctx, d, scene, icons, i, 0);
}

// Best-effort quaternion -> euler (degrees) for the inspector rotation fields.
static void quat_to_euler_deg(const em::quat& q, float out[3]) {
    float sinp = 2.0f * (q.w * q.x + q.y * q.z);
    float cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    float siny = 2.0f * (q.w * q.y - q.z * q.x);
    siny = siny > 1.0f ? 1.0f : (siny < -1.0f ? -1.0f : siny);
    float sinr = 2.0f * (q.w * q.z + q.x * q.y);
    float cosr = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float R2D = 57.2957795f;
    out[0] = std::atan2(sinp, cosp) * R2D;
    out[1] = std::asin(siny) * R2D;
    out[2] = std::atan2(sinr, cosr) * R2D;
}

// One transform component: small round [-] SVG, a draggable/typeable value
// (no oversized stepper arrows), and a small round [+] SVG. True if changed.
static bool num_field(nk_context* ctx, Editor::Impl* d, const char* lbl,
                      const char* id, float* v, float step) {
    const float fh = ctx->style.font->height;
    const float before = *v;
    nk_layout_row_template_begin(ctx, fh + 8.0f);
    nk_layout_row_template_push_static(ctx, fh * 1.4f);
    nk_layout_row_template_push_static(ctx, fh + 4.0f);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, fh + 4.0f);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, lbl, NK_TEXT_LEFT);
    nk_style_button sb = ctx->style.button;
    ctx->style.button.rounding = (fh + 8.0f) * 0.45f;
    if (nk_button_symbol(ctx, NK_SYMBOL_MINUS)) *v -= step;
    bool active = (d->edit_id == id);
    char local[40]; char* buf; int len;
    if (active) { buf = d->edit_buf; len = (int)std::strlen(buf); }
    else { std::snprintf(local, sizeof local, "%.4g", *v); buf = local; len = (int)std::strlen(local); }
    nk_flags fl = nk_edit_string(ctx, NK_EDIT_FIELD, buf, &len, 39, nk_filter_float);
    if (len < 0) len = 0;
    if (len > 39) len = 39;
    buf[len] = 0;
    if (fl & NK_EDIT_ACTIVE) {
        if (!active) { std::strncpy(d->edit_buf, buf, 39); d->edit_buf[39] = 0; d->edit_id = id; }
        *v = (float)std::atof(d->edit_buf);
    } else if (active) {
        *v = (float)std::atof(d->edit_buf);
        d->edit_id.clear();
    }
    if (nk_button_symbol(ctx, NK_SYMBOL_PLUS)) *v += step;
    ctx->style.button = sb;
    return *v != before;
}

// Custom collapsible header: a small hand-drawn triangle + title, clickable.
// Replaces nk_tree (whose collapse symbol is sized to the row -> huge on 4K).
static bool section(nk_context* ctx, Editor::Impl* d, const char* title) {
    auto it = d->sections.find(title);
    bool open = (it == d->sections.end()) ? true : it->second;
    nk_layout_row_dynamic(ctx, ctx->style.font->height + 8.0f, 1);
    struct nk_rect b;
    if (nk_widget(&b, ctx) == NK_WIDGET_INVALID) { d->sections[title] = open; return open; }
    struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
    const struct nk_input* in = &ctx->input;
    bool hover = nk_input_is_mouse_hovering_rect(in, b);
    if (hover && nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT)) open = !open;
    d->sections[title] = open;
    const float fh = ctx->style.font->height;
    nk_color hb = hover ? nk_rgb(64, 66, 72) : nk_rgb(50, 52, 58);
    nk_fill_rect(canvas, b, 3.0f, hb);
    float cx = b.x + fh * 0.5f, cy = b.y + b.h * 0.5f, t = fh * 0.20f;
    nk_color ac = nk_rgb(205, 205, 210);
    if (open) nk_fill_triangle(canvas, cx - t, cy - t * 0.5f, cx + t, cy - t * 0.5f, cx, cy + t * 0.8f, ac);
    else      nk_fill_triangle(canvas, cx - t * 0.5f, cy - t, cx - t * 0.5f, cy + t, cx + t * 0.8f, cy, ac);
    struct nk_rect tr = nk_rect(b.x + fh, b.y, b.w - fh, b.h);
    nk_draw_text(canvas, tr, title, (int)std::strlen(title), ctx->style.font, hb, nk_rgb(228, 228, 232));
    return open;
}

static void body_inspector(nk_context* ctx, Editor::Impl* d, Scene& scene,
                           Project& project, const IconSet& icons) {
    const float S = ctx->style.font->height / 18.0f;
    if (d->selected < 0 || d->selected >= (int)scene.entities.size()) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "(no selection)", NK_TEXT_LEFT);
        return;
    }
    Entity& e = scene.entities[d->selected];

    icon_label(ctx, icons, kind_icon(e.kind), e.name.c_str(), 0);

    // --- instance kind selector -------------------------------------------
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Instance Kind", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 26, 1);
    // NUL-separated (see kTemplateNames note): nk_combo_string splits on '\0'.
    static const char kKindNames[] = "Empty\0Renderable\0RigidBody\0StaticCollider\0Camera\0";
    int kind_i = (int)e.kind;
    int new_kind = nk_combo_string(ctx, kKindNames, kind_i, 5, (int)(26 * S), nk_vec2(220.0f * S, 160.0f * S));
    if (new_kind != kind_i) d->queue({EditorAction::SetKind, "", new_kind});

    // --- transform ---------------------------------------------------------
    if (section(ctx, d, "Transform")) {
        const float lblh = ctx->style.font->height + 4.0f;
        nk_layout_row_dynamic(ctx, lblh, 1);
        nk_label(ctx, "Position", NK_TEXT_LEFT);
        num_field(ctx, d, "x", "px", &e.transform.position.x, 0.1f);
        num_field(ctx, d, "y", "py", &e.transform.position.y, 0.1f);
        num_field(ctx, d, "z", "pz", &e.transform.position.z, 0.1f);

        nk_layout_row_dynamic(ctx, lblh, 1);
        nk_label(ctx, "Rotation", NK_TEXT_LEFT);
        if (d->insp_euler_for != d->selected) {
            quat_to_euler_deg(e.transform.rotation, d->insp_euler);
            d->insp_euler_for = d->selected;
        }
        bool rch = false;
        rch |= num_field(ctx, d, "x", "rx", &d->insp_euler[0], 1.0f);
        rch |= num_field(ctx, d, "y", "ry", &d->insp_euler[1], 1.0f);
        rch |= num_field(ctx, d, "z", "rz", &d->insp_euler[2], 1.0f);
        if (rch) {
            const float D2R = 0.01745329252f;
            e.transform.rotation = em::quat::from_euler(d->insp_euler[0] * D2R,
                                                        d->insp_euler[1] * D2R,
                                                        d->insp_euler[2] * D2R);
        }

        nk_layout_row_dynamic(ctx, lblh, 1);
        nk_label(ctx, "Scale", NK_TEXT_LEFT);
        num_field(ctx, d, "x", "sx", &e.transform.scale.x, 0.1f);
        num_field(ctx, d, "y", "sy", &e.transform.scale.y, 0.1f);
        num_field(ctx, d, "z", "sz", &e.transform.scale.z, 0.1f);
        if (e.transform.scale.x < 0.001f) e.transform.scale.x = 0.001f;
        if (e.transform.scale.y < 0.001f) e.transform.scale.y = 0.001f;
        if (e.transform.scale.z < 0.001f) e.transform.scale.z = 0.001f;
    }

    // --- camera settings ---------------------------------------------------
    if (e.kind == InstanceKind::Camera && section(ctx, d, "Camera")) {
        nk_layout_row_dynamic(ctx, ctx->style.font->height + 4.0f, 1);
        nk_label(ctx, "Field of View", NK_TEXT_LEFT);
        num_field(ctx, d, "fov", "cfov", &e.fov, 1.0f);
        if (e.fov < 10.0f)  e.fov = 10.0f;
        if (e.fov > 170.0f) e.fov = 170.0f;
    }

    // --- material ----------------------------------------------------------
    if (section(ctx, d, "Material")) {
        icon_label(ctx, icons, "material", e.material.c_str(), 0);
        const auto& mats = project.materials();
        nk_layout_row_dynamic(ctx, 22, 1);
        for (auto& m : mats)
            if (nk_button_label(ctx, m.stem.c_str())) d->queue({EditorAction::AssignMaterial, m.stem, 0});
    }

    // --- physics / collider -----------------------------------------------
    if (section(ctx, d, "Physics")) {
        nk_layout_row_template_begin(ctx, ctx->style.font->height + 6.0f);
        nk_layout_row_template_push_static(ctx, ctx->style.font->height + 6.0f);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        nk_image(ctx, icons.get(e.body.valid() ? "rigidbody" : "collider"));
        nk_labelf(ctx, NK_TEXT_LEFT, "Body: %s   Collider: %s",
                  e.body.valid() ? "active" : "none", collider_name(e.collider));

        nk_layout_row_dynamic(ctx, ctx->style.font->height + 4.0f, 1);
        nk_label(ctx, "Collider", NK_TEXT_LEFT);
        static const char kColliders[] = "None\0Box\0Sphere\0Capsule\0Convex\0";
        nk_layout_row_dynamic(ctx, ctx->style.font->height + 10.0f, 1);
        int ci = (int)e.collider;
        int nci = nk_combo_string(ctx, kColliders, ci, 5, (int)(28 * S), nk_vec2(220.0f * S, 240.0f * S));
        if (nci != ci) d->queue({EditorAction::GenerateCollider, "", nci});
    }

    // --- gameplay script ---------------------------------------------------
    if (section(ctx, d, "Script")) {
        icon_label(ctx, icons, e.has_script() ? "script_ok" : "script_none",
                   e.has_script() ? e.script.c_str() : "(none)", 0);
        std::string cur = e.has_script() ? e.script : "(none)";
        nk_layout_row_dynamic(ctx, ctx->style.font->height + 10.0f, 1);
        if (nk_combo_begin_label(ctx, cur.c_str(), nk_vec2(260.0f * S, 320.0f * S))) {
            nk_layout_row_dynamic(ctx, ctx->style.font->height + 8.0f, 1);
            if (nk_combo_item_label(ctx, "(none)", NK_TEXT_LEFT)) d->queue({EditorAction::AssignScript, "", 0});
            for (auto& a : project.scripts()) {
                if (a.name == "engine_api.hpp") continue;
                if (nk_combo_item_label(ctx, a.name.c_str(), NK_TEXT_LEFT)) d->queue({EditorAction::AssignScript, a.stem, 0});
            }
            nk_combo_end(ctx);
        }
    }

    // --- visibility + delete ----------------------------------------------
    nk_layout_row_dynamic(ctx, ctx->style.font->height + 8.0f, 1);
    int vis = e.visible ? 1 : 0; nk_checkbox_label(ctx, "Visible", &vis); e.visible = vis;
    nk_layout_row_dynamic(ctx, 8.0f, 1); nk_spacing(ctx, 1);   // gap so it can't clip Delete
    nk_layout_row_dynamic(ctx, ctx->style.font->height + 12.0f, 1);
    {
        nk_style_button saved = ctx->style.button;
        ctx->style.button.normal = nk_style_item_color(nk_rgb(150, 55, 55));
        ctx->style.button.hover  = nk_style_item_color(nk_rgb(190, 70, 70));
        ctx->style.button.active = nk_style_item_color(nk_rgb(120, 40, 40));
        if (nk_button_label(ctx, "Delete"))
            d->queue({EditorAction::DeleteSelected, "", 0});
        ctx->style.button = saved;
    }
}

static void body_console(nk_context* ctx, Editor::Impl* d) {
    const float S = ctx->style.font->height / 18.0f;
    nk_layout_row_template_begin(ctx, 24.0f * S);
    nk_layout_row_template_push_static(ctx, 60.0f * S);
    nk_layout_row_template_push_static(ctx, 120.0f * S);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    if (nk_button_label(ctx, "Clear")) d->console.clear();
    nk_label(ctx, "build command:", NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, d->build_cmd, sizeof(d->build_cmd), nk_filter_default);

    nk_layout_row_dynamic(ctx, 120.0f * S, 1);
    const auto lines = d->console.snapshot();
    if (nk_group_begin(ctx, "console_scroll", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, ctx->style.font->height + 2.0f, 1);  // one log line per font-height row (was 13px -> overlapped)
        for (const std::string& l : lines) {
            nk_color col = nk_rgb(205, 205, 205);
            if (l.find("error") != std::string::npos || l.find("FAILED") != std::string::npos || l.find("Error") != std::string::npos)
                col = nk_rgb(235, 90, 80);
            else if (l.rfind("[build]", 0) == 0 || l.rfind("[run]", 0) == 0 || l.rfind("[edit]", 0) == 0 || l.rfind("[project]", 0) == 0)
                col = nk_rgb(110, 200, 235);
            else if (l.find("SUCCESS") != std::string::npos)
                col = nk_rgb(120, 220, 120);
            nk_label_colored(ctx, l.c_str(), NK_TEXT_LEFT, col);
        }
        nk_group_end(ctx);
    }
    // While a build streams in, follow the tail so the latest output stays visible.
    if (d->build.running) {
        nk_uint sy = (nk_uint)(lines.size() * (size_t)(ctx->style.font->height + 2.0f));
        nk_group_set_scroll(ctx, "console_scroll", 0, sy);
    }
}

static void body_editor(nk_context* ctx, Editor::Impl* d, Project& proj,
                        const IconSet& icons, float body_h) {
    const float S = ctx->style.font->height / 18.0f;
    // toolbar row: file name + template combo + insert
    const float trh = ctx->style.font->height + 8.0f;
    nk_layout_row_template_begin(ctx, trh);
    nk_layout_row_template_push_static(ctx, 64.0f * S);
    nk_layout_row_template_push_static(ctx, 280.0f * S);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, "Script", NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, d->script_name, nk_vec2(320.0f * S, 360.0f * S))) {
        nk_layout_row_dynamic(ctx, trh, 1);
        for (const auto& sc : proj.scripts()) {
            if (sc.name == "engine_api.hpp") continue;
            if (nk_combo_item_label(ctx, sc.name.c_str(), NK_TEXT_LEFT)) load_into_editor(d, sc.path);
        }
        nk_combo_end(ctx);
    }
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, d->script_name, sizeof(d->script_name), nk_filter_default);

    // ---- editable + syntax-coloured source widget ----
    // Sync script_buf -> code editor whenever the buffer changed externally
    // (Insert / snippets / open_script / template). The text comparison also
    // covers the resync_code() flag flips above.
    {
        const char* buf = d->script_buf.data();
        if (!d->code_loaded || d->code.text != buf) {
            d->code.set_text(buf);
            d->code_loaded = true;
        }
    }
    float ed_h = body_h - (ctx->style.font->height + 8.0f) /*top row*/ - (ctx->style.font->height + 18.0f) /*compile*/ - 16.0f;
    if (ed_h < 80.0f) ed_h = 80.0f;
    nk_layout_row_dynamic(ctx, ed_h, 1);
    if (d->mono) nk_style_push_font(ctx, d->mono);
    const bool code_changed = d->code.draw(ctx, ed_h);
    if (d->mono) nk_style_pop_font(ctx);
    if (code_changed) {
        // pull edits back into the canonical char buffer (truncate to capacity)
        std::string& t = d->code.text;
        if (t.size() >= d->script_buf.size()) t.resize(d->script_buf.size() - 1);
        std::memcpy(d->script_buf.data(), t.data(), t.size());
        d->script_buf[t.size()] = 0;
    }

    // Ctrl+S saves the current script (same path as the Save button).
    {
        const bool* ks = SDL_GetKeyboardState(nullptr);
        const bool want = (ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL]) && ks[SDL_SCANCODE_S];
        if (want && !d->save_key_down) {
            std::string path = proj.new_script(d->script_name, d->script_buf.data());
            if (!path.empty()) d->console.add("[edit] saved " + path + "  (Ctrl+S)");
        }
        d->save_key_down = want;
    }

    // auto-include toggle: ON suggests all C++ funcs and adds the #include on Tab;
    // OFF only suggests functions whose header is already in the file.
    nk_layout_row_dynamic(ctx, ctx->style.font->height + 6.0f, 1);
    {
        int ai = d->code.auto_include ? 1 : 0;
        nk_checkbox_label(ctx, ai ? "Auto-include: ON (Tab adds #include)" : "Auto-include: OFF (only your libs)", &ai);
        d->code.auto_include = ai;
    }
    // build status / error line (real-time after pressing Compile)
    {
        std::string status; nk_color sc = nk_rgb(150, 200, 130);
        if (d->build.running) { status = "Checking / compiling..."; sc = nk_rgb(220, 200, 110); }
        else if (d->build.finished) {
            if (d->build.last_rc == 0) status = "OK - compiles cleanly";
            else {
                sc = nk_rgb(235, 95, 85);
                for (const std::string& l : d->console.snapshot())
                    if (l.find("error:") != std::string::npos) { status = l; break; }
                if (status.empty()) status = "Failed (see Console)";
            }
        }
        if (!status.empty()) {
            nk_layout_row_dynamic(ctx, ctx->style.font->height + 4.0f, 1);
            nk_label_colored(ctx, status.c_str(), NK_TEXT_LEFT, sc);
        }
    }
    // compile row only (Ctrl+S saves; the Editor toolbar button closes it)
    nk_layout_row_static(ctx, ctx->style.font->height + 8.0f, (int)(170.0f * S), 1);
    {
        nk_style_button saved; style_green(ctx, saved);
        if (nk_button_label(ctx, d->build.running ? "Compiling..." : "Compile")) {
            std::string path = proj.new_script(d->script_name, d->script_buf.data());
            if (!path.empty()) {
                std::string stem = std::filesystem::path(path).stem().string();
                std::string out = proj.dir_build() + "/" + stem + ".so";
                d->build.start("g++ -std=c++20 -fPIC -shared -I" + shq(proj.dir_scripts()) +
                               " " + shq(path) + " -o " + shq(out), &d->console);
            }
        }
        ctx->style.button = saved;
    }
}

// ----------------------------------------------------------------------------- draw
void Editor::draw(nk_context* ctx, int sw, int sh, Scene& scene, Camera& cam,
                  PhysicsWorld& physics, Project& project, float fps, const IconSet& icons) {
    Impl* d = p;
    d->project = &project;
    // UI scale derived from the baked font height (font is baked at 18 * display
    // scale), so the whole editor tracks the monitor's content scale.
    const float S = (ctx->style.font && ctx->style.font->height > 0.0f)
                    ? ctx->style.font->height / 18.0f : 1.0f;
    (void)cam; (void)physics;
    // Cleaner transform fields: drop the oversized +/- stepper arrows (dragging
    // the value and double-click-to-type still work). Give checkboxes a clearly
    // visible checkmark fill so toggles read as on/off.
    ctx->style.property.sym_left  = NK_SYMBOL_NONE;
    ctx->style.property.sym_right = NK_SYMBOL_NONE;
    ctx->style.checkbox.normal        = nk_style_item_color(nk_rgb(58, 58, 62));
    ctx->style.checkbox.hover         = nk_style_item_color(nk_rgb(74, 74, 80));
    ctx->style.checkbox.cursor_normal = nk_style_item_image(icons.get("check"));
    ctx->style.checkbox.cursor_hover  = nk_style_item_image(icons.get("check"));
    ctx->style.checkbox.border        = 1.0f;
    ctx->style.checkbox.border_color  = nk_rgb(160, 160, 160);

    if (d->run_after_build && d->build.finished && !d->build.running) {
        if (d->build.last_rc == 0) launch_detached(d->run_path, &d->console);
        d->run_after_build = false;
    }

    // ===== menu bar / toolbar (stays free-floating across the top 84px) =====
    if (nk_begin(ctx, "Toolbar", nk_rect(0, 0, (float)sw, 84.0f * S),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_layout_set_min_row_height(ctx, ctx->style.font->height + 6.0f);
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 26.0f * S, 5);
        nk_layout_row_push(ctx, 55.0f * S);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(240.0f * S, 200.0f * S))) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_menu_item_label(ctx, "New Scene", NK_TEXT_LEFT)) d->queue({EditorAction::ResetScene, "", 0});
            if (nk_menu_item_label(ctx, "Save Scene  (Ctrl+S)", NK_TEXT_LEFT)) d->queue({EditorAction::SaveScene, "", 0});
            if (nk_menu_item_label(ctx, "Load Scene", NK_TEXT_LEFT)) d->queue({EditorAction::LoadScene, "", 0});
            if (nk_menu_item_label(ctx, "Open Project Folder", NK_TEXT_LEFT)) project.open_in_file_manager();
            nk_menu_end(ctx);
        }
        nk_layout_row_push(ctx, 70.0f * S);
        if (nk_menu_begin_label(ctx, "Create", NK_TEXT_LEFT, nk_vec2(220.0f * S, 280.0f * S))) {
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_label(ctx, "Primitives", NK_TEXT_LEFT);
            if (nk_menu_item_label(ctx, "Cube", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnPrimitive, "", 0});
            if (nk_menu_item_label(ctx, "Sphere", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnPrimitive, "", 1});
            if (nk_menu_item_label(ctx, "Plane", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnPrimitive, "", 2});
            nk_label(ctx, "Instances", NK_TEXT_LEFT);
            if (nk_menu_item_label(ctx, "Empty", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::Empty});
            if (nk_menu_item_label(ctx, "Renderable", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::Renderable});
            if (nk_menu_item_label(ctx, "RigidBody", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::RigidBody});
            if (nk_menu_item_label(ctx, "StaticCollider", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::StaticCollider});
            if (nk_menu_item_label(ctx, "Camera", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::Camera});
            if (nk_menu_item_label(ctx, "Collection", NK_TEXT_LEFT)) d->queue({EditorAction::SpawnInstance, "", (int)InstanceKind::Empty});
            nk_menu_end(ctx);
        }
        nk_layout_row_push(ctx, 70.0f * S);
        if (nk_menu_begin_label(ctx, "Build", NK_TEXT_LEFT, nk_vec2(220.0f * S, 200.0f * S))) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_menu_item_label(ctx, "Build", NK_TEXT_LEFT)) d->build.start(d->build_cmd, &d->console);
            if (nk_menu_item_label(ctx, "Build & Run", NK_TEXT_LEFT)) { d->build.start(d->build_cmd, &d->console); d->run_after_build = true; }
            if (nk_menu_item_label(ctx, "Run", NK_TEXT_LEFT)) launch_detached(d->run_path, &d->console);
            nk_menu_end(ctx);
        }
        nk_layout_row_push(ctx, 80.0f * S);
        if (nk_menu_begin_label(ctx, "Window", NK_TEXT_LEFT, nk_vec2(260.0f * S, 240.0f * S))) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_menu_item_label(ctx, "Script Editor", NK_TEXT_LEFT)) d->show_editor = true;
            if (nk_menu_item_label(ctx, "Open Debug Script", NK_TEXT_LEFT)) {
                load_into_editor(d, project.debug_script_path());
            }
            nk_menu_end(ctx);
        }
        if (nk_menu_begin_label(ctx, "View", NK_TEXT_LEFT, nk_vec2(300.0f * S, 380.0f * S))) {
            nk_layout_row_dynamic(ctx, ctx->style.font->height + 8.0f, 1);
            int vg = d->show_gizmos ? 1 : 0;    nk_checkbox_label(ctx, "Gizmos", &vg);    d->show_gizmos = vg;
            int vc = d->show_colliders ? 1 : 0; nk_checkbox_label(ctx, "Colliders", &vc); d->show_colliders = vc;
            int sn = d->snap_on ? 1 : 0; nk_checkbox_label(ctx, "Snapping (Ctrl = /8)", &sn); d->snap_on = sn;
            nk_label(ctx, "Move / Scale step", NK_TEXT_LEFT);
            nk_property_float(ctx, "#mv", 0.01f, &d->snap_grid, 100.0f, 0.05f, 0.05f);
            nk_label(ctx, "Rotate step (deg)", NK_TEXT_LEFT);
            nk_property_float(ctx, "#ro", 1.0f, &d->snap_angle, 90.0f, 1.0f, 1.0f);
            nk_menu_end(ctx);
        }
        nk_menubar_end(ctx);

        // toolbar: Play/Stop (in-editor simulation), Build, Editor, gizmo modes
        nk_layout_row_static(ctx, ctx->style.font->height + 10.0f, (int)(112 * S), 7);
        {
            nk_style_button saved = ctx->style.button;
            if (!d->playing) {
                style_green(ctx, saved);
                if (nk_button_label(ctx, "Play")) d->queue({EditorAction::TogglePlay, "", 0});
            } else {
                ctx->style.button.normal = nk_style_item_color(nk_rgb(175, 70, 60));
                ctx->style.button.hover  = nk_style_item_color(nk_rgb(205, 95, 85));
                ctx->style.button.text_normal = nk_rgb(255, 255, 255);
                if (nk_button_label(ctx, "Stop")) d->queue({EditorAction::TogglePlay, "", 0});
            }
            ctx->style.button = saved;
        }
        if (nk_button_label(ctx, d->build.running ? "Building" : "Build")) d->build.start(d->build_cmd, &d->console);
        if (nk_button_label(ctx, "Editor")) d->show_editor = !d->show_editor;
        // gizmo-mode toggles (highlight the active one)
        nk_style_button gsaved = ctx->style.button;
        ctx->style.button.image_padding = nk_vec2(10.0f, 10.0f);   // shrink icon, leave room for label
        auto gizmo_btn = [&](const char* icon, const char* label, int mode) {
            nk_style_item norm = ctx->style.button.normal;
            if (d->gizmo_mode == mode)
                ctx->style.button.normal = nk_style_item_color(nk_rgb(70, 110, 180));
            if (nk_button_image_label(ctx, icons.get(icon), label, NK_TEXT_LEFT)) d->gizmo_mode = mode;
            ctx->style.button.normal = norm;
        };
        gizmo_btn("gizmo_move",   "Move",   0);
        gizmo_btn("gizmo_rotate", "Rotate", 1);
        gizmo_btn("gizmo_scale",  "Scale",  2);
        ctx->style.button = gsaved;
        {   // Local / Global axis-space toggle
            nk_style_button saved = ctx->style.button;
            if (d->gizmo_local)
                ctx->style.button.normal = nk_style_item_color(nk_rgb(70, 110, 180));
            if (nk_button_label(ctx, d->gizmo_local ? "Local" : "Global")) d->gizmo_local = !d->gizmo_local;
            ctx->style.button = saved;
        }

        nk_layout_row_static(ctx, ctx->style.font->height + 2.0f, (float)sw, 1);
        nk_labelf(ctx, NK_TEXT_LEFT, "  %s   |   %.0f FPS   |   %zu entities   |   gizmo: %s",
                  project.name().c_str(), fps, scene.entities.size(),
                  d->gizmo_mode == 0 ? "move" : d->gizmo_mode == 1 ? "rotate" : "scale");
    }
    nk_end(ctx);

    // ===== simple FIXED panels (no dock: stable, no tab/splitter glitches) =====
    const float TOP = 84.0f * S, LW = 300.0f * S, RW = 300.0f * S, BH = 200.0f * S;
    const float Wf = (float)sw, Hf = (float)sh;
    if (d->playing) {
        // ===== Game view: hide the editor panels; the running game fills the screen.
        d->vp_x = 0.0f; d->vp_y = TOP; d->vp_w = Wf; d->vp_h = Hf - TOP;
    } else {
        // ===== simple FIXED panels (no dock: stable, no tab/splitter glitches) =====
        d->vp_x = LW; d->vp_y = TOP;
        d->vp_w = Wf - LW - RW; d->vp_h = Hf - TOP - BH;
        if (d->vp_w < 40.0f) d->vp_w = 40.0f;
        if (d->vp_h < 40.0f) d->vp_h = 40.0f;
        const nk_flags PANEL = NK_WINDOW_TITLE | NK_WINDOW_BORDER;
        auto panel = [&](const char* nm, struct nk_rect r) -> bool {
            nk_window_set_bounds(ctx, nm, r);
            bool open = nk_begin(ctx, nm, r, PANEL);
            if (open) nk_layout_set_min_row_height(ctx, ctx->style.font->height + 6.0f);
            return open;
        };
        const float lh = (Hf - TOP - BH) * 0.5f;
        if (panel("Assets",    nk_rect(0, TOP, LW, lh)))                  body_assets(ctx, d, project, icons);
        nk_end(ctx);
        if (panel("Outliner",  nk_rect(0, TOP + lh, LW, lh)))             body_outliner(ctx, d, scene, icons);
        nk_end(ctx);
        if (panel("Inspector", nk_rect(Wf - RW, TOP, RW, Hf - TOP - BH))) body_inspector(ctx, d, scene, project, icons);
        nk_end(ctx);
        if (panel("Console",   nk_rect(0, Hf - BH, Wf, BH)))              body_console(ctx, d);
        nk_end(ctx);
    }

    // ===== Script Editor: a large CENTERED window over the viewport =====
    // The dock Center zone is reserved for the 3D view, so the code editor
    // floats over it as a movable / resizable editor tab — a much better spot
    // than sharing the bottom output strip. Opened on demand (menu / New Script
    // / Edit), closed with the body's Close button.
    if (d->show_editor) {
        // Fill the center viewport area (replaces the 3D preview while editing).
        float ex = d->vp_x, ey = d->vp_y, ew = d->vp_w, eh = d->vp_h;
        if (ew < 200.0f || eh < 150.0f) { ex = (float)sw*0.15f; ey = 92.0f; ew = (float)sw*0.70f; eh = (float)sh - ey - 16.0f; }
        nk_flags wf = NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR;
        nk_window_set_bounds(ctx, "Script Editor", nk_rect(ex, ey, ew, eh));  // pin over the viewport
        if (nk_begin(ctx, "Script Editor", nk_rect(ex, ey, ew, eh), wf)) {
            nk_layout_set_min_row_height(ctx, ctx->style.font->height + 6.0f);
            body_editor(ctx, d, project, icons, eh - 44.0f);
        }
        nk_end(ctx);
    }
}

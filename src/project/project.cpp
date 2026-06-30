#include "pch.hpp"
#include "project/project.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------- starter content
static const char* kStarterMaterials[][2] = {
    {"default.tgmat",
     "// default.tgmat — neutral grey surface\n"
     "Material\n{\n    shader        \"mesh\"\n"
     "    g_vColorTint  [0.80 0.80 0.82 1.0]\n"
     "    g_flMetalness 0.0\n    g_flRoughness 0.55\n    g_flAmbient   0.18\n}\n"},
    {"crate.tgmat",
     "// crate.tgmat — warm orange crate\n"
     "Material\n{\n    shader        \"mesh\"\n"
     "    g_vColorTint  [0.85 0.52 0.24 1.0]\n"
     "    g_flMetalness 0.05\n    g_flRoughness 0.45\n    g_flAmbient   0.16\n}\n"},
    {"grid.tgmat",
     "// grid.tgmat — desaturated blue-grey floor\n"
     "Material\n{\n    shader        \"mesh\"\n"
     "    g_vColorTint  [0.32 0.36 0.42 1.0]\n"
     "    g_flMetalness 0.0\n    g_flRoughness 0.7\n    g_flAmbient   0.22\n}\n"},
};

static const char* kEngineApi =
    "#pragma once\n"
    "// engine_api.hpp — gameplay scripting API available to every project script.\n"
    "// Define on_start / on_update in your script; the engine calls them.\n"
    "#include <cstdio>\n\n"
    "namespace game {\n"
    "    struct Vec3 { float x = 0, y = 0, z = 0; };\n"
    "    struct Entity;                       // opaque handle to a scene object\n\n"
    "    Vec3  get_position(Entity& e);\n"
    "    void  set_position(Entity& e, Vec3 p);\n"
    "    float time_seconds();\n"
    "    void  log(const char* fmt, ...);\n"
    "}\n\n"
    "// Declare a script component. Define GAME_ON_START / GAME_ON_UPDATE below it.\n"
    "#define GAME_SCRIPT(Name) namespace Name\n";

static const char* kTemplateSpinner =
    "// Spinner.cpp — template gameplay script.\n"
    "// Rotates / moves the entity it is attached to.\n"
    "#include \"engine_api.hpp\"\n\n"
    "using namespace game;\n\n"
    "// Called once when the entity spawns.\n"
    "void on_start(Entity& self) {\n"
    "    log(\"Spinner started\\n\");\n"
    "}\n\n"
    "// Called every frame. dt is the frame time in seconds.\n"
    "void on_update(Entity& self, float dt) {\n"
    "    Vec3 p = get_position(self);\n"
    "    p.y += 0.5f * dt;              // drift upward as a demo\n"
    "    set_position(self, p);\n"
    "}\n";

static const char* kTemplateDebug =
    "// Debug.cpp — scratch debug script.\n"
    "// This file is created automatically and is meant to be auto-opened while\n"
    "// you are testing the engine. Drop throwaway gameplay logic here, hit\n"
    "// compile, and watch the console — nothing here ships with the game.\n"
    "#include \"../engine_api.hpp\"\n\n"
    "using namespace game;\n\n"
    "// Runs once when this script is (re)loaded.\n"
    "void on_start(Entity& self) {\n"
    "    log(\"[debug] Debug.cpp loaded at t=%.2f\\n\", time_seconds());\n"
    "}\n\n"
    "// Runs every frame while debugging. dt is the frame time in seconds.\n"
    "void on_update(Entity& self, float dt) {\n"
    "    // log(\"[debug] dt=%.4f\\n\", dt);   // uncomment to trace frame timing\n"
    "}\n";

// ----------------------------------------------------------------------------- helpers
static bool write_file(const fs::path& p, const std::string& content) {
    std::ofstream f(p);
    if (!f) return false;
    f << content;
    return true;
}

// POSIX single-quote a path so it survives /bin/sh word-splitting. Any embedded
// single quote is closed, escaped, and reopened:  it's  ->  'it'\''s'.
static std::string shell_quoted(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += '\'';
    return out;
}

std::string Project::default_root() {
    if (const char* env = std::getenv("ENGINE_PROJECT"); env && *env) return env;
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/WildEngineProjects/Starter";
    return "WildEngineProjects/Starter";
}

std::string Project::name() const {
    return root_.empty() ? "(none)" : fs::path(root_).filename().string();
}

// ----------------------------------------------------------------------------- open / scaffold
bool Project::open(const std::string& root) {
    root_ = root.empty() ? default_root() : root;
    try {
        bool fresh = !fs::exists(fs::path(root_) / "project.json");
        if (fresh) {
            if (!scaffold()) { warnln("project: failed to scaffold %s", root_.c_str()); }
            println("Created new project at %s", root_.c_str());
        } else {
            println("Opened project %s", root_.c_str());
        }
    } catch (const std::exception& ex) {
        warnln("project: %s", ex.what());
        return false;
    }
    refresh();
    return true;
}

bool Project::scaffold() {
    fs::create_directories(dir_models());
    fs::create_directories(dir_materials());
    fs::create_directories(dir_scripts());
    fs::create_directories(dir_scenes());
    fs::create_directories(dir_build());

    write_file(fs::path(root_) / "project.json",
        "{\n  \"name\": \"" + name() + "\",\n  \"engine\": \"wild\",\n  \"version\": 1\n}\n");
    for (auto& m : kStarterMaterials)
        write_file(fs::path(dir_materials()) / m[0], m[1]);
    write_file(fs::path(dir_scripts()) / "engine_api.hpp", kEngineApi);
    write_file(fs::path(dir_scripts()) / "Spinner.cpp", kTemplateSpinner);

    // A scratch "debug" script in its own subfolder, auto-opened while testing.
    fs::create_directories(fs::path(dir_scripts()) / "debug");
    write_file(fs::path(debug_script_path()), kTemplateDebug);

    write_file(fs::path(root_) / "README.md",
        "# " + name() + "\n\nA game project made with the engine. Drop `.glb` models in "
        "`assets/models/`, edit `.tgmat` materials in `assets/materials/`, and write gameplay "
        "C++ in `scripts/`.\n");
    return true;
}

// ----------------------------------------------------------------------------- scan
static void scan_dir(const std::string& dir, std::vector<std::string> exts,
                     std::vector<AssetEntry>& out, AssetEntry::Kind kind) {
    out.clear();
    try {
        if (!fs::exists(dir)) return;
        for (auto& de : fs::directory_iterator(dir)) {
            if (!de.is_regular_file()) continue;
            std::string ext = de.path().extension().string();
            bool match = false;
            for (auto& e : exts) if (ext == e) { match = true; break; }
            if (!match) continue;
            AssetEntry a;
            a.name = de.path().filename().string();
            a.stem = de.path().stem().string();
            a.path = de.path().string();
            a.kind = kind;
            out.push_back(std::move(a));
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(), [](const AssetEntry& a, const AssetEntry& b){ return a.name < b.name; });
}

// Scan scripts/ AND one level of its subdirectories (e.g. scripts/debug/) for
// .cpp files. Nested scripts keep a name that includes the subfolder
// ("debug/Debug.cpp") so the editor can tell them apart from top-level ones.
static void scan_scripts(const std::string& root, std::vector<AssetEntry>& out) {
    out.clear();
    auto collect = [&](const fs::path& dir, const std::string& prefix) {
        if (!fs::exists(dir)) return;
        for (auto& de : fs::directory_iterator(dir)) {
            if (!de.is_regular_file()) continue;
            if (de.path().extension() != ".cpp") continue;
            AssetEntry a;
            a.name = prefix + de.path().filename().string();
            a.stem = de.path().stem().string();
            a.path = de.path().string();
            a.kind = AssetEntry::Script;
            out.push_back(std::move(a));
        }
    };
    try {
        fs::path base(root);
        collect(base, "");
        if (fs::exists(base)) {
            for (auto& de : fs::directory_iterator(base)) {
                if (!de.is_directory()) continue;
                collect(de.path(), de.path().filename().string() + "/");
            }
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(),
              [](const AssetEntry& a, const AssetEntry& b){ return a.name < b.name; });
}

void Project::refresh() {
    scan_dir(dir_models(),    {".glb", ".gltf"}, models_,    AssetEntry::Model);
    scan_dir(dir_materials(), {".tgmat"},          materials_, AssetEntry::Material);
    scan_scripts(dir_scripts(), scripts_);
}

Project::ScriptStatus Project::script_status(const std::string& script_path) const {
    try {
        fs::path cpp(script_path);
        if (!fs::exists(cpp)) return ScriptStatus::None;
        // Compiled artifact is build/<stem>.so next to the other build output.
        fs::path so = fs::path(dir_build()) / (cpp.stem().string() + ".so");
        if (!fs::exists(so)) return ScriptStatus::None;
        auto cpp_t = fs::last_write_time(cpp);
        auto so_t  = fs::last_write_time(so);
        return cpp_t > so_t ? ScriptStatus::Outdated : ScriptStatus::Compiled;
    } catch (...) {
        return ScriptStatus::None;
    }
}

void Project::open_in_file_manager() const {
    if (root_.empty()) return;
    // Launch the desktop file manager on the project root, detached, swallowing
    // its stdout/stderr so it never pollutes the engine console.
    std::system(("xdg-open " + shell_quoted(root_) + " >/dev/null 2>&1 &").c_str());
}

std::string Project::new_script(const std::string& base_name, const std::string& body) {
    std::string nm = base_name.empty() ? "NewScript" : base_name;
    if (nm.size() < 4 || nm.substr(nm.size() - 4) != ".cpp") nm += ".cpp";
    fs::path p = fs::path(dir_scripts()) / nm;
    if (!write_file(p, body)) { warnln("project: cannot write %s", p.string().c_str()); return {}; }
    refresh();
    return p.string();
}

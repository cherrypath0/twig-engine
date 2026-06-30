#pragma once
// A "project" is a game made WITH the engine — it lives in its own folder,
// completely separate from the engine install. Models, materials, scripts and
// scenes belong to the project, not to the engine source tree.
//
//   <project>/
//     project.json          // metadata
//     assets/models/*.glb
//     assets/materials/*.tgmat
//     scripts/*.cpp          // gameplay C++ (see scripts/engine_api.hpp)
//     scenes/*.scene
//     build/                 // compiled script artifacts
//
#include <string>
#include <vector>

struct AssetEntry {
    enum Kind { Model, Material, Script, Scene, Other };
    std::string name;   // file name (e.g. "crate.tgmat")
    std::string stem;   // without extension (e.g. "crate")
    std::string path;   // absolute path
    Kind        kind = Other;
};

class Project {
public:
    // Open the project at `root`, scaffolding a starter project if it is empty.
    // If `root` is empty, a default location under the user's home is used.
    bool open(const std::string& root);

    bool valid() const { return !root_.empty(); }
    const std::string& root() const { return root_; }
    std::string name() const;

    std::string dir_models()    const { return root_ + "/assets/models"; }
    std::string dir_materials() const { return root_ + "/assets/materials"; }
    std::string dir_scripts()   const { return root_ + "/scripts"; }
    std::string dir_scenes()    const { return root_ + "/scenes"; }
    std::string dir_build()     const { return root_ + "/build"; }

    // The auto-generated debug script, created by scaffold(). The editor can
    // auto-open this while testing so there is always a scratch script to run.
    std::string debug_script_path() const { return dir_scripts() + "/debug/Debug.cpp"; }

    // Open the project's root folder in the OS file manager (xdg-open).
    void open_in_file_manager() const;

    void refresh();   // re-scan the asset folders
    const std::vector<AssetEntry>& models()    const { return models_; }
    const std::vector<AssetEntry>& materials() const { return materials_; }
    const std::vector<AssetEntry>& scripts()   const { return scripts_; }

    // Create a new C++ script from a template; returns its path ("" on failure).
    std::string new_script(const std::string& base_name, const std::string& body);

    // Compile state of a project script, derived from file modification times:
    //   None      no build/<stem>.so exists (script was never compiled)
    //   Outdated  the .cpp is newer than its .so (needs a recompile)
    //   Compiled  the .so is at least as new as the .cpp (up to date)
    enum class ScriptStatus { None, Compiled, Outdated };
    ScriptStatus script_status(const std::string& script_path) const;

    static std::string default_root();   // $ENGINE_PROJECT or ~/WildEngineProjects/Starter

private:
    std::string root_;
    std::vector<AssetEntry> models_, materials_, scripts_;

    bool scaffold();   // create dirs + starter content
};

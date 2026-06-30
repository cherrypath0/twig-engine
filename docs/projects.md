# Projects (game content vs. the engine)

The engine is a **tool**. A *game* is a **project** that lives in its own folder,
completely separate from the engine install — so your models, materials and
scripts are never mixed into the engine source tree.

```
<project>/
  project.json            # metadata
  assets/models/*.glb     # your models  (auto-loaded on start)
  assets/materials/*.tgmat   # your materials
  scripts/
    engine_api.hpp        # gameplay scripting API (auto-generated)
    *.cpp                 # your gameplay C++
  scenes/
  build/                  # compiled script .so artifacts
```

## Where is my project?

On first launch the engine scaffolds a starter project and opens it:

- default: `~/WildEngineProjects/Starter`
- override: set `ENGINE_PROJECT=/path/to/MyGame` before launching.

The starter project comes with the three sample `.tgmat` materials, an
`engine_api.hpp`, and a `Spinner.cpp` template script.

## The editor

- **Assets panel** — browse the project's *Models / Materials / Scripts*.
  - Models → **Add** instantiates the `.glb` in front of the camera.
  - Materials → **Assign** applies the `.tgmat` to the selected entity (also from
    the Inspector's *Set material*).
  - Scripts → **Edit** opens the file in the Script Editor.
- **Create menu** — Cube / Sphere / Plane primitives (spawned in front of the
  camera and auto-selected).
- **Script Editor** — write C++ with template insertion, a snippet palette,
  a syntax-**coloured preview**, **Save** (writes into `<project>/scripts/`) and
  **Save + Compile** (compiles to `<project>/build/<name>.so`, streaming the
  compiler output into the Console).

## Adding content

Drop a `.glb` into `<project>/assets/models/` (then **Refresh** in the Assets
panel), add `.tgmat` files to `<project>/assets/materials/`, or create scripts from
the Script Editor. Nothing touches the engine itself.

## Scripting API

Project scripts include `engine_api.hpp` and may define `on_start(Entity&)` /
`on_update(Entity&, float dt)`. Compilation produces a `.so` in the project's
`build/`. (Hot-loading those `.so`s into the running game is the next milestone;
today the compile validates your code and produces the artifact.)

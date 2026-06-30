# Twig Engine — Documentation

**Twig Engine** is a small **Source 2-inspired** game engine written in C++20 on top of the
**SDL3 GPU API** (Vulkan/SPIR-V backend). It features an immediate-mode editor
UI built with **Nuklear**, **GLB (glTF binary)** model loading, a custom
**`.tgmat` material format**, GLSL shaders compiled to SPIR-V, and rigid-body
physics powered by **Jolt Physics**.

> Build system is **Task** (go-task) (there is no CMake). All third-party single-header
> libraries are vendored; Jolt is compiled from source into a cached static lib.

---

## 1. Building & running

Prerequisites: a C++20 compiler (`g++`/`clang++`), `glslc` (shaderc) for shader
compilation, and the SDL3 shared library (vendored under `libs/SDL3`).

```bash
tools/fetch-deps.sh   # once: fetch nuklear, cgltf, stb_image, Jolt
go-task            # dev build  -> build/main-dev   (compiles shaders + Jolt as needed)
go-task run        # build, then launch the dev binary
go-task build CHANNEL=live   # optimised "live" channel (isDev == false)
go-task windows    # best-effort Windows cross-compile via mingw-w64 (physics stubbed)
go-task clean      # remove engine objects + binaries + compiled shaders
go-task distclean  # also drop the cached Jolt static library
go-task --list   # list all tasks
```

The first build also compiles Jolt Physics (153 translation units) into
`build/libJolt.a`. That archive is **cached** — subsequent builds reuse it, so
only your engine code recompiles. Use `go-task jolt` to (re)build just the library.

### Channels
`cflags.txt` maps a channel name to compile defines:

| channel | define     | `isDev` | meaning                  |
|---------|------------|---------|--------------------------|
| `dev`   | `-Dccdev`  | `true`  | extended debug logging   |
| `live`  | `-Dcclive` | `false` | release-ish              |

> **Vulkan validation layers** are opt-in (some SDL3 + Mesa stacks double-free a
> driver allocation in `SDL_Quit`, so the engine skips the global quit and exits
> cleanly instead). Enable validation with `ENGINE_GPU_DEBUG=1 ./build/main-dev`.

---

## 2. Controls (in the editor)

| Input                | Action                                   |
|----------------------|------------------------------------------|
| **Right-mouse hold** | Enter fly-cam (relative mouse look)      |
| **W A S D**          | Move (while looking)                     |
| **Q / E**            | Move down / up                           |
| **Shift**            | Move faster                              |
| **Esc**              | Quit                                     |

Editor panels (all Nuklear, drag/resize them):
- **Toolbar / menu bar** — File, Build, Scene menus + the green **Build & Run**
  button.
- **Outliner** — every entity in the scene; click to select.
- **Inspector** — edit the selected entity's position / scale / visibility.
- **Stats** — FPS, physics backend + live body count, camera position.
- **Console** — streams the output of the asynchronous `go-task` build, with an
  editable build command field.

### The Build button
The **Build & Run** button runs `go-task` in a background thread, streaming
compiler output live into the Console panel, and (for *Build & Run*) launches
the freshly built binary when the compile succeeds. The exact command is
editable in the Console (`go-task CHANNEL=dev -j4` by default).

---

## 3. Architecture

```
main.cpp
  └── Engine                         src/core/engine.*
        ├── Renderer                 src/renderer/render.*      (3D forward pass)
        ├── NuklearBackend           src/ui/nuklear_backend.*   (UI -> SDL3 GPU)
        ├── Editor                   src/ui/editor.*            (panels + Build)
        ├── Scene                    src/scene/scene.hpp        (entities)
        ├── Camera                   src/camera/camera.hpp      (fly cam)
        ├── PhysicsWorld             src/physics/physics.*      (Jolt façade)
        └── MaterialLibrary (map)    src/material/material.*    (.tgmat)
GPU helpers     src/gpu/gpu.*        (buffer/texture upload, SPIR-V loading)
Model loading   src/gltf/model.*     (cgltf -> GpuMesh, procedural primitives)
Math            modules/math/        (vec/mat/quat, projection)
3rd-party impl  src/core/thirdparty.cpp  (nuklear, cgltf, stb_image)
```

### Frame flow
```
input_begin → poll SDL events (UI + camera) → input_end
physics.update(dt) → sync entity transforms from bodies
editor.draw(...)   → build Nuklear widgets, service the background build
renderer.render(): acquire cmd + swapchain
    ui.prepare(cmd)              // copy pass: upload UI geometry
    pass 1: clear + depth, draw every visible entity (mesh + material)
    pass 2: color LOAD, draw the UI overlay
    submit
```

### Rendering notes
- SDL3 GPU's NDC is lower-left `(-1,-1)`, depth `[0,1]`, and SDL handles the
  Vulkan Y-flip internally — so `em::perspective` (Y-up) and the UI ortho need
  **no manual flips**.
- The mesh pipeline uses a `D32_FLOAT` depth target, `LESS` depth test, and
  `CULL_NONE` (robust against winding differences).
- Shader resource binding follows SDL3 GPU's model: vertex uniforms in **set 1**,
  fragment samplers in **set 2**, fragment uniforms in **set 3**.

---

## 4. Subsystems

- [`material-format.md`](material-format.md) — the custom `.tgmat` format.
- [`models.md`](models.md) — GLB loading and procedural meshes.
- [`shaders.md`](shaders.md) — the GLSL pipeline and SDL3 GPU binding rules.
- [`physics.md`](physics.md) — the Jolt integration.

---

## 5. Project layout

```
Taskfile.yaml         build system (replaces CMake)
cflags.txt           channel -> compile-define table
shaders/*.vert|frag  GLSL sources -> *.spv via glslc
assets/materials/    *.tgmat material files
assets/models/       drop a *.glb here to auto-load it
modules/*/include/   header-only engine modules (pch, output, config, rng, math)
libs/                vendored deps (SDL3, nuklear, cgltf, JoltPhysics, json)
src/                 engine source
build/               objects, the Jolt lib, compiled binaries
```

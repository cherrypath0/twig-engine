# CLAUDE.md — engine guide for future Claude sessions

This file is the orientation guide for any Claude (or human) working on this
repository. Read it before touching code. It explains the architecture, the
frame loop, the build system, the gotchas that will bite you, and the recipes
for adding features. It is deliberately accurate to the **actual code** — when
in doubt, the headers listed below are the source of truth.

> **Twig Engine** — a small Source 2-inspired C++20 game engine on the **SDL3 GPU API**
> (Vulkan/SPIR-V backend), with a **Nuklear** immediate-mode editor, **GLB**
> model loading, a custom **`.tgmat`** material format, and **Jolt Physics**.
>
> Deeper notes live in [`docs/engine-internals.md`](docs/engine-internals.md).
> Subsystem docs live in [`docs/`](docs/README.md).

---

## 1. The golden rules (read first)

1. **Every `.cpp` starts with `#include "pch.hpp"`.** The PCH
   (`modules/pch/include/pch.hpp`) pulls in `<string>`, `<filesystem>`, SDL3,
   `config.hpp`, `output.hpp`, `rng.hpp`. Do not re-include those.
2. **Never include `<nuklear.h>` directly.** Include
   [`src/ui/nuklear_config.hpp`](src/ui/nuklear_config.hpp) for any `nk_*` use —
   it sets the required `NK_*` defines before the single header. The actual
   Nuklear implementation is compiled exactly once in `src/core/thirdparty.cpp`.
3. **Logging is via `output.hpp` macros**, not `printf`/`std::cout`:
   `println(fmt, ...)`, `warnln(fmt, ...)`, and the dev-only `devprintln` /
   `devwarnln` (compiled out unless `isDev`). They take printf-style args.
4. **The build is `go-task`, not CMake.** There is no `CMakeLists.txt`. See §4.
5. **Physics types never leak.** `src/physics/physics.hpp` is a façade; no Jolt
   header appears outside `physics.cpp`. Keep it that way.
6. **The math lib is `namespace em`** in `modules/math/include/math.hpp`
   (`vec2/3/4`, `mat4`, `quat`, `look_at`, `perspective`, `ortho`, `radians`,
   `normalize`, `cross`, `dot`, `compose`). Right-handed, depth `[0,1]`.

---

## 2. Architecture

```
main.cpp
  └── Engine                         src/core/engine.*        (owns everything)
        ├── Renderer                 src/renderer/render.*     (3D forward pass + UI overlay)
        ├── NuklearBackend           src/ui/nuklear_backend.*  (UI geometry -> SDL3 GPU)
        ├── Editor                   src/ui/editor.*           (panels + queued actions)
        ├── Scene                    src/scene/scene.hpp       (flat entity list)
        ├── Camera                   src/camera/camera.*       (fly cam, view/proj)
        ├── PhysicsWorld             src/physics/physics.*     (Jolt façade / stub)
        ├── Project                  src/project/project.*     (the game, separate from engine)
        └── materials_ (map)         src/material/material.*   (.tgmat registry)
GPU helpers     src/gpu/gpu.*        (buffer/texture upload, SPIR-V loading)
Model loading   src/gltf/model.*     (cgltf -> GpuMesh, procedural primitives)
Math            modules/math/        (em:: vec/mat/quat, projection)
3rd-party impl  src/core/thirdparty.cpp  (nuklear + cgltf + stb_image implementations)
```

### Engine owns everything
`Engine` (`src/core/engine.hpp`) holds the `SDL_Window*`, the `SDL_GPUDevice*`,
and every subsystem by value. It drives the main loop and is the **only** code
allowed to mutate the GPU/scene/physics in response to editor requests. The
editor cannot touch the GPU — it raises `EditorAction`s that `Engine` drains.

### Scene / Entity (`src/scene/scene.hpp`)
A `Scene` is a flat `std::vector<Entity>` plus a `std::vector<GpuMesh>` it owns,
a sun direction and a sky color. An `Entity` is data-oriented:
`{ std::string name; Transform transform; int mesh; std::string material;
PhysicsBody body; bool visible; }`. `mesh` indexes into `Scene::meshes`
(`-1` = none); `material` is a key into the engine's material map. `Transform`
is `{ position, rotation (quat), scale }` with `matrix() == em::compose(...)`.

### Camera (`src/camera/camera.hpp`)
A fly camera: `{ position, yaw, pitch, fov_deg }` with `view()`,
`proj(aspect)`, and basis accessors `forward()/right()/up()`. `proj` uses
`em::perspective` (right-handed, `[0,1]` depth).

### Material (`src/material/material.hpp`, `.tgmat`)
Source 2-style text format parsed by `material::load_tgmat`. Keys accept Source 2
names (`g_vColorTint`, `g_flRoughness`, `g_tColor`, ...) or friendly aliases.
`material::resolve_gpu` loads `color_texture` (stb_image) into a GPU texture, or
falls back to the renderer's 1×1 white texture. Engine stores materials in an
`std::unordered_map<std::string, Material>` keyed by name; entities reference it
by string. See [`docs/material-format.md`](docs/material-format.md).

### PhysicsWorld (`src/physics/physics.hpp`, Jolt façade)
Opaque `void* impl_`. `add_box/add_sphere` return a `PhysicsBody { uint32_t id }`
whose `valid()` gates whether physics drives the entity. `get_transform` reads a
body back into `em::vec3 pos` + `em::quat rot`. `backend()` returns `"Jolt"` or
`"stub"` (the Windows build stubs physics). Call `optimize()` once after building
the scene. See [`docs/physics.md`](docs/physics.md).

### Project (`src/project/project.hpp`, the game)
A **project is a game made WITH the engine**, living in its own folder, fully
separate from the engine source tree (see §6). `Project::open(root)` scaffolds a
starter project (materials, `engine_api.hpp`, `Spinner.cpp`, a `scripts/debug/`
scratch script) on first open, then `refresh()` scans the asset folders.
See [`docs/projects.md`](docs/projects.md).

### gpu helpers (`src/gpu/gpu.hpp`)
Thin SDL3 GPU plumbing: `upload_buffer` (blocking copy-pass upload),
`create_texture_rgba`, `create_white_texture`, `create_linear_sampler`,
`load_spirv(dev, path, stage, num_samplers, num_uniform_buffers)`
(`stage` 0=vertex, 1=fragment — mirrors `SDL_GPUShaderStage` without leaking SDL
into headers).

### GLB loading (`src/gltf/model.hpp`)
cgltf parses a `.glb` into `MeshData { std::vector<Vertex>; std::vector<uint32_t>
indices; ... }` where `Vertex { em::vec3 pos, normal; em::vec2 uv; }`. Uploaded
into a `GpuMesh { SDL_GPUBuffer* vbo, ibo; uint32_t index_count; em::vec3
bounds_min, bounds_max; }`. Procedural cube/sphere/plane primitives live here
too. See [`docs/models.md`](docs/models.md).

---

## 3. The frame loop

`Engine::run()` (`src/core/engine.cpp`) is the canonical loop. Each frame:

```
handle_input(dt):                      // src/core/engine.cpp
    ui_.input_begin();
    while (SDL_PollEvent(&e)) {
        ui_.handle_event(e);           // feed Nuklear first
        ... camera / quit handling ... // then the engine reacts
    }
    ui_.input_end();
physics_.update(dt);                   // step Jolt
sync_physics();                        // copy body transforms -> entity transforms
editor_.draw(ctx, w, h, scene_, camera_, physics_, project_, fps);  // build UI
process_editor_actions();              // drain EditorAction queue (spawn/assign/delete/...)
renderer_.render(scene_, camera_, materials_, ui_);                 // draw
```

`Renderer::render` (`src/renderer/render.cpp`) does, in order:
1. `SDL_AcquireGPUCommandBuffer`, then `SDL_WaitAndAcquireGPUSwapchainTexture`.
2. `ui.prepare(cmd)` — **outside any render pass** — copy-pass-uploads this
   frame's UI geometry. Must precede every render pass.
3. `ensure_depth(w, h)` — lazily (re)creates the `D32_FLOAT` depth target on
   resize.
4. **Pass 1 (3D):** color = swapchain (CLEAR to `sky_color`), depth = CLEAR to
   1.0. Bind the mesh pipeline; for each visible entity push the vertex UBO
   (`model`, `mvp = proj*view*model`) and fragment UBO (base color, light dir,
   camera pos, `{metalness, roughness, ambient, 0}`), bind VBO/IBO + the
   material texture (or white) + sampler, and `SDL_DrawGPUIndexedPrimitives`.
5. **Pass 2 (UI):** color = swapchain with `LOAD` (no depth). `ui.render(...)`.
6. `SDL_SubmitGPUCommandBuffer`.

---

## 4. Build system (`go-task` / `Taskfile.yaml`)

```bash
tools/fetch-deps.sh          # once: fetch nuklear, cgltf, stb_image, Jolt
go-task                      # dev build  -> build/main-dev
go-task run                  # build, then launch
go-task build CHANNEL=live   # optimised live channel (isDev == false)
go-task shaders              # glslc: shaders/*.vert|frag -> *.spv
go-task jolt                 # build the cached Jolt static lib
go-task windows              # mingw-w64 cross-compile (physics stubbed)
go-task clean / distclean
go-task --list
```

- **Channels** (`cflags.txt`): `dev -> -Dccdev` (`isDev==true`),
  `live -> -Dcclive` (`isDev==false`).
- **Cached Jolt lib:** the first build compiles Jolt from source into
  `build/libJolt.a`, then reuses it. `go-task distclean` drops it.
- **Shaders:** `glslc` compiles `shaders/*.vert|frag` to `*.spv`, only when
  stale. The `*.spv` files are loaded at runtime via `gpu::load_spirv`.

### Three gotchas that WILL bite you (also in `~/.claude` memory)
1. **No `SDL_Quit()`.** `Engine::shutdown` deliberately omits `SDL_Quit()`:
   on the SDL3 + Mesa Vulkan stack it double-frees a driver allocation during
   teardown. The engine exits cleanly without it. Do not "fix" this by adding it.
2. **SDL3 soname + bundled version.** Run against the **bundled** SDL3 (matching
   the vendored headers), not the system copy. The Taskfile creates the soname
   symlink `libs/SDL3/lib/libSDL3.so.0 -> libSDL3.so` and sets an `$ORIGIN`
   rpath so the loader picks the right `.so`.
3. **`-DJPH_NO_DEBUG`.** The engine must be compiled with `-DENGINE_WITH_JOLT
   -DJPH_NO_DEBUG` so its Jolt config matches the `NDEBUG`-built `libJolt.a`;
   otherwise you get a `JPH::AssertFailed` link error.

### Compiling a single TU by hand
When you need to compile-check one file in isolation (the Taskfile globs all of
`src/`), use the engine's exact flags, e.g.:
```
g++ -std=c++20 -Wall -Wextra -Wno-unused-parameter -msse4.2 -mpopcnt \
  -Dccdev -DENGINE_WITH_JOLT -DJPH_NO_DEBUG \
  -Isrc -Imodules/pch/include -Imodules/config/include -Imodules/output/include \
  -Imodules/rng/include -Imodules/math/include -Ilibs/SDL3/include \
  -Ilibs/json/include -Ilibs/nuklear -Ilibs/cgltf -Ilibs/JoltPhysics -Ilibs/nanosvg \
  -c src/<file>.cpp -o /tmp/<file>.o
```

---

## 5. SDL3 GPU binding model + NDC convention

This is the most common source of "why is my shader wrong" bugs.

- **Binding sets** (declared in GLSL, matched by the push calls):
  - **set 0** — vertex sampled textures (unused here)
  - **set 1** — **vertex uniform buffers** → `SDL_PushGPUVertexUniformData(cmd, slot, ...)`
  - **set 2** — **fragment sampled textures** → `SDL_BindGPUFragmentSamplers(pass, slot, ...)`
  - **set 3** — **fragment uniform buffers** → `SDL_PushGPUFragmentUniformData(cmd, slot, ...)`

  The mesh vertex shader declares `layout(set = 1, binding = 0) uniform VertexUBO`,
  and the fragment shader declares its sampler in set 2 / its UBO in set 3.
  `gpu::load_spirv` is told the sampler/uniform counts so SDL can size the layout.
- **NDC / Y-flip:** SDL3 GPU's NDC is lower-left, depth `[0,1]`, and SDL handles
  the Vulkan Y-flip internally. So `em::perspective` (Y-up, `[0,1]`) and the UI
  `em::ortho` need **no manual flip**.
- **Mesh pipeline:** `D32_FLOAT` depth, `LESS` depth test, `CULL_NONE`.

---

## 6. Project-vs-engine separation

Keep these mental models distinct:

- **Engine** = this repo's `src/`, shaders, modules — the runtime + editor.
- **Project** = a *game* in its own folder elsewhere on disk, managed by the
  `Project` class. Default location is `$ENGINE_PROJECT` or
  `~/WildEngineProjects/Starter`. Layout:
  ```
  <project>/
    project.json              metadata
    assets/models/*.glb       drop a GLB here to load it
    assets/materials/*.tgmat     material files
    scripts/*.cpp              gameplay C++ (see scripts/engine_api.hpp)
    scripts/debug/Debug.cpp    auto-created scratch script, opened while testing
    scenes/*.scene
    build/                     compiled script artifacts (<stem>.so)
  ```

Gameplay scripts are project assets, **not** engine source — they compile to
`build/<stem>.so` inside the project. `Project::script_status(path)` reports
`None` / `Outdated` / `Compiled` by comparing `<script>.cpp` mtime against
`build/<stem>.so`. `Project::open_in_file_manager()` opens the project root in
the OS file manager (xdg-open). See [`docs/projects.md`](docs/projects.md).

---

## 7. Recipes: how to add things

### Add an editor panel
Panels are built in `src/ui/editor.cpp` (owned by the integrator). A panel is a
Nuklear window inside `Editor::draw`. If it needs the engine to *do* something
(touch the GPU/scene/physics), **do not** do it in the editor — extend the
`EditorAction` enum in `src/ui/editor.hpp`, push the action during `draw`, and
handle it in `Engine::process_editor_actions`. Never call GPU code from the UI.

### Add a material key
Extend `Material` in `src/material/material.hpp`, parse the new key (Source 2
name + alias) in `material::load_tgmat` (`material.cpp`), pass it through to the
fragment UBO in `Renderer::render`, and consume it in `shaders/mesh.frag`.
Recompile shaders (`go-task shaders`). Document it in `docs/material-format.md`.

### Add a collider type
Add an `add_*` method to `PhysicsWorld` (`physics.hpp` façade + `physics.cpp`
Jolt impl **and** the stub impl), returning a `PhysicsBody`. Wire spawning into
`Engine` (e.g. a new `EditorAction` / `spawn_*`). Keep all Jolt types inside
`physics.cpp`.

### Add a gameplay script
Use `Project::new_script(name, body)` to create `scripts/<Name>.cpp` from a
template (it `refresh()`es afterward). The scratch `scripts/debug/Debug.cpp`
exists for throwaway logic. Scripts compile to `build/<stem>.so`; the editor can
show compile state via `Project::script_status`.

### Add a shader
Drop `shaders/<name>.vert|frag`, reference it from a material's `shader` key,
load it with `gpu::load_spirv` (passing correct sampler/uniform counts), and
respect the set 1/2/3 binding model from §5.

---

## 8. File ownership note for parallel work

Several files are "integrator-owned" and edited by a coordinating agent:
`src/ui/editor.cpp`, `src/core/engine.cpp`, `src/renderer/render.cpp`,
`src/main.cpp`, and `Taskfile.yaml`. If you are a subagent building one module,
stay in your assigned files and describe the integration in your result rather
than editing those files yourself.

---

## 9. Where to look next

- [`docs/README.md`](docs/README.md) — documentation index, controls, build.
- [`docs/engine-internals.md`](docs/engine-internals.md) — render passes, UI
  texture binding for icons, resource lifetimes (the deep dive).
- [`docs/material-format.md`](docs/material-format.md),
  [`docs/models.md`](docs/models.md),
  [`docs/shaders.md`](docs/shaders.md),
  [`docs/physics.md`](docs/physics.md),
  [`docs/projects.md`](docs/projects.md).

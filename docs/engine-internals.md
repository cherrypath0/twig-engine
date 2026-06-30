# Engine internals (deep dive)

Companion to the top-level [`CLAUDE.md`](../CLAUDE.md) and the docs
[index](README.md). This page covers the render passes in detail, how UI icons
bind their own textures, and the lifetimes of GPU resources. Everything here is
checked against the real code in `src/renderer/render.cpp`,
`src/ui/nuklear_backend.cpp`, and `src/gpu/gpu.cpp`.

---

## 1. Render passes

`Renderer::render(scene, cam, materials, ui)` runs **two render passes** per
frame against the swapchain texture, bracketed by one copy pass for UI geometry.

### 1.0 Acquire
```
cmd  = SDL_AcquireGPUCommandBuffer(dev)
ok   = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &w, &h)
```
If the swapchain isn't ready (minimised / zero-size), the command buffer is
submitted empty and the frame is skipped — `render` still returns `true`
(only a genuine GPU error returns `false`).

### 1.1 Copy pass (UI upload) — outside any render pass
`ui.prepare(cmd)` converts Nuklear's command list into vertex/index data and
uploads it through an `SDL_GPUCopyPass`. **This must happen before
`SDL_BeginGPURenderPass`** — you cannot run a copy pass while a render pass is
open. The depth target is also (re)created here on resize via `ensure_depth(w,h)`.

### 1.2 Pass 1 — 3D scene (color + depth)
- **Color target:** the swapchain texture, `LOADOP_CLEAR` to `scene.sky_color`,
  `STOREOP_STORE`.
- **Depth target:** `depth_` (`D32_FLOAT`), `clear_depth = 1.0`,
  `LOADOP_CLEAR`, `STOREOP_DONT_CARE` (depth isn't needed after the frame).
  Stencil load/store are `DONT_CARE`.
- Bind `mesh_pipeline_`, then per visible entity (`visible && mesh in range &&
  vbo/ibo/index_count valid`):
  - **Vertex UBO** (set 1, slot 0) `MeshVertexUBO { mat4 mvp; mat4 model; }` —
    note `mvp = proj * view * model`, pushed with
    `SDL_PushGPUVertexUniformData(cmd, 0, ...)`.
  - **Fragment UBO** (set 3, slot 0) `MeshFragUBO { vec4 baseColor; vec4
    lightDir; vec4 cameraPos; vec4 params; }`. `baseColor = material.color_tint`,
    `lightDir.xyz = scene.sun_dir` with `lightDir.w` flagging texture use,
    `params = {metalness, roughness, ambient, 0}`. Pushed with
    `SDL_PushGPUFragmentUniformData(cmd, 0, ...)`.
  - **Geometry:** bind VBO at vertex buffer slot 0, IBO as `INDEXELEMENTSIZE_32BIT`.
  - **Texture** (set 2, slot 0): `material.tex` or the renderer's `white_`
    fallback, plus the shared linear `sampler_`, via
    `SDL_BindGPUFragmentSamplers(pass, 0, ...)`.
  - `SDL_DrawGPUIndexedPrimitives(pass, index_count, 1, 0, 0, 0)`.

The vertex layout is three attributes from one interleaved buffer:
`location 0 = pos (FLOAT3)`, `1 = normal (FLOAT3)`, `2 = uv (FLOAT2)`, matching
`Vertex` via `offsetof`.

### 1.3 Pass 2 — UI overlay (color only)
- **Color target:** the swapchain again, but `LOADOP_LOAD` (preserve the 3D
  result) + `STOREOP_STORE`. **No depth target** — the UI draws on top.
- `ui.render(cmd, pass, w, h)` draws the prepared geometry (see §2).

### 1.4 Submit
`SDL_SubmitGPUCommandBuffer(cmd)`. One command buffer covers copy + both passes.

> **NDC reminder:** SDL3 GPU handles the Vulkan Y-flip internally and uses
> `[0,1]` depth, so `em::perspective` and the UI's `em::ortho` need no manual
> flips. The mesh pipeline uses a `LESS` depth test and `CULL_NONE`.

---

## 2. UI rendering & per-command texture binding (icons)

`NuklearBackend::render` is where immediate-mode UI becomes GPU draws, and where
**icons** get their own textures.

- The UI vertex shader takes one uniform: an `em::ortho(0, w, h, 0)` projection
  pushed to vertex set 1, slot 0. Pixel space, origin top-left.
- Geometry lives in two persistent buffers (`p->vbo`, `p->ibo`), refilled each
  frame in `prepare`; indices are **16-bit** (`INDEXELEMENTSIZE_16BIT`).
- Drawing walks `nk_draw_foreach`. For **each draw command**:
  - The clip rect is **clamped to the framebuffer** before becoming an
    `SDL_Rect` scissor — Nuklear emits a "no clip" rect of essentially infinite
    size that would otherwise trip SDL's GPU validation layer. Commands whose
    clamped scissor is empty are skipped.
  - **Texture binding is per-command:** `dcmd->texture.ptr` is cast straight to
    `SDL_GPUTexture*`. Text and solid geometry carry the font-atlas handle;
    `nk_image` icons stash their own `SDL_GPUTexture*` in `texture.ptr`. If a
    command has no texture, it falls back to the font atlas so a null handle
    never reaches the GPU. The shared linear sampler is bound alongside it.
  - `SDL_DrawGPUIndexedPrimitives(pass, elem_count, 1, offset, 0, 0)`, advancing
    `offset` by `elem_count`.
- After the loop the scissor is reset to the full framebuffer so a following
  pass isn't accidentally clipped.

**To show a custom icon in a panel:** create an `SDL_GPUTexture` (e.g.
`gpu::create_texture_rgba`), wrap its pointer in an `nk_image` whose handle
`ptr` is that texture, and draw it with `nk_image`/`nk_image_color`. The backend
binds it automatically for that command — no pipeline change needed, because the
UI fragment shader already samples set 2, slot 0.

---

## 3. Resource lifetimes

### Owned by `Renderer` (created in `init`, freed in `shutdown`)
- `mesh_pipeline_` — the 3D graphics pipeline (mesh shaders + vertex layout +
  depth/cull state).
- `depth_` — the `D32_FLOAT` depth texture. **Recreated on resize** by
  `ensure_depth(w, h)` when `w/h` differ from `depth_w_/depth_h_`; the old one
  is released first.
- `white_` — a 1×1 white texture used as the "no texture" material fallback.
- `sampler_` — one shared linear sampler for all mesh draws.
- `swap_fmt_` — the swapchain format, queried once and handed to the UI backend
  so its pipeline targets the same format.

### Owned by `NuklearBackend`
- `pipeline`, `vbo`, `ibo`, a transfer buffer (`xfer`), the font atlas texture
  (`font_tex`), and its own `sampler`. The vertex/index buffers persist across
  frames and are refilled; they grow as needed. All released in `shutdown`.

### Owned by `Scene`
- `Scene::meshes` holds `GpuMesh { vbo, ibo, ... }`. These GPU buffers are
  created when geometry is loaded (GLB or procedural primitive via the gpu
  helpers / model loader) and live as long as the mesh entry. Entities reference
  meshes by **index**, so removing/reordering meshes must keep `Entity::mesh`
  indices valid (the engine appends rather than reindexing).

### Owned by `Material`
- `Material::tex` is filled by `material::resolve_gpu` (stb_image → GPU texture)
  or left pointing at the white fallback. Materials live in the engine's
  `materials_` map for the process lifetime.

### The no-`SDL_Quit` lifetime quirk
`Engine::shutdown` releases the device, window, and subsystem resources but
**does not call `SDL_Quit()`** — on the SDL3 + Mesa Vulkan stack that
double-frees a driver allocation during teardown. The process exits cleanly
without it; the OS reclaims everything. Do not add `SDL_Quit()` back.

---

## 4. Shader uniform structs (must match GLSL exactly)

The C++ UBO structs in `render.cpp` are laid out to match `std140`-style GLSL
blocks. When you add a field:

- Mirror it in **both** the C++ struct and the GLSL `uniform` block, in the same
  order, honoring `vec4` alignment (pack scalars into `vec4` lanes — that's why
  `params` is a `vec4` carrying three floats + a pad).
- Tell `gpu::load_spirv` the correct `num_samplers` / `num_uniform_buffers` for
  the stage so SDL sizes the descriptor layout.
- Recompile shaders with `go-task shaders` (the `.spv` files are what load at
  runtime, not the `.vert`/`.frag`).

See [`shaders.md`](shaders.md) for the binding-set rules and
[`material-format.md`](material-format.md) for how material keys reach the
fragment UBO.

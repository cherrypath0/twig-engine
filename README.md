# Twig Engine

**Twig Engine** — a small **Source 2-inspired** game engine in **C++20** on the **SDL3 GPU API**
(Vulkan/SPIR-V), with an immediate-mode **Nuklear** editor, **GLB** model
loading, a custom **`.tgmat`** material format, GLSL→SPIR-V shaders, and
**Jolt Physics**. Built with **Task** (go-task; no CMake).

```bash
tools/fetch-deps.sh   # once: fetch nuklear, cgltf, stb_image, Jolt
go-task          # build (compiles shaders + Jolt as needed) -> build/main-dev
go-task run      # build and launch
```

Right-drag to fly (WASD/QE, Shift to boost). The editor has an Outliner,
Inspector, Stats and a Console, plus a green **Build & Run** button that runs
`go-task` in the background and streams the output into the Console.

📖 **Full documentation:** [`docs/README.md`](docs/README.md)
— [materials](docs/material-format.md) · [models](docs/models.md) ·
[shaders](docs/shaders.md) · [physics](docs/physics.md)

## Features

- **Task build system** (replaces CMake); cross-compiles a Windows `.exe` too.
- **Nuklear UI** rendered through a custom SDL3-GPU backend.
- **GLB/glTF** loading via cgltf + procedural cube/sphere/plane primitives.
- **Custom `.tgmat` materials** (Source 2-style KeyValues; `g_tColor`, tint,
  metalness, roughness, …).
- **GLSL** shaders compiled to SPIR-V by `glslc`.
- **Jolt Physics** compiled from source into a cached static lib, behind a clean
  façade (with a stub fallback for platforms without it).

## Layout

| Path | What |
|------|------|
| `Taskfile.yaml` | build system |
| `src/` | engine source (core, renderer, ui, gltf, material, physics, scene, camera, gpu) |
| `modules/` | header-only modules (pch, output, config, rng, math) |
| `shaders/` | GLSL sources compiled to `.spv` |
| `assets/` | `.tgmat` materials, drop `.glb` models in `assets/models/` |
| `libs/` | vendored deps (SDL3, nuklear, cgltf, JoltPhysics, json) |
| `docs/` | documentation |

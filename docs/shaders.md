# Shaders (GLSL → SPIR-V)

Shaders are authored in **GLSL** (`#version 450`) under `shaders/` and compiled
to SPIR-V by `glslc` as part of the build:

```yaml
# Taskfile.yaml — the `shaders` task
- glslc shaders/mesh.vert -o shaders/mesh.vert.spv   # for every *.vert / *.frag
```

So `shaders/mesh.vert` → `shaders/mesh.vert.spv`, loaded at runtime by
`gpu::load_spirv`. SDL3's GPU device is created with `SDL_GPU_SHADERFORMAT_SPIRV`
and the Vulkan backend.

## The shaders

| File         | Stage    | Purpose                                            |
|--------------|----------|----------------------------------------------------|
| `mesh.vert`  | vertex   | transforms vertices by `mvp`, passes world normal  |
| `mesh.frag`  | fragment | tint × optional texture + directional + ambient    |
| `ui.vert`    | vertex   | Nuklear: ortho-projects screen-space UI verts      |
| `ui.frag`    | fragment | Nuklear: vertex colour × font/atlas texture        |

## SDL3 GPU resource binding model

This is the part that trips people up. With the SPIR-V/Vulkan backend, SDL3 GPU
assigns descriptor **sets** by stage and resource kind. Declare them explicitly:

| Resource                     | GLSL qualifier                  | Bound with                        |
|------------------------------|---------------------------------|-----------------------------------|
| Vertex uniform buffer        | `layout(set = 1, binding = 0)`  | `SDL_PushGPUVertexUniformData`     |
| Fragment sampler / texture   | `layout(set = 2, binding = 0)`  | `SDL_BindGPUFragmentSamplers`      |
| Fragment uniform buffer      | `layout(set = 3, binding = 0)`  | `SDL_PushGPUFragmentUniformData`   |

(Vertex samplers would be set 0; this engine doesn't use them.)

The C++ uniform structs in [`src/core/types.hpp`](../src/core/types.hpp)
(`MeshVertexUBO`, `MeshFragUBO`) mirror the GLSL blocks exactly.

## Clip space

SDL3 GPU normalised device coordinates: lower-left `(-1,-1)`, upper-right
`(1,1)`, depth `[0,1]`, and SDL converts for Vulkan's Y-down NDC automatically.
Practical consequence: the Y-up `em::perspective` and the UI `em::ortho(0, w, h,
0)` are both correct **without** any manual Y-flip.

## Adding a shader

1. Write `shaders/foo.vert` / `shaders/foo.frag`.
2. `go-task shaders` (or just `go-task`) compiles them to `.spv`.
3. Load with `gpu::load_spirv(dev, "shaders/foo.vert.spv", stage, numSamplers,
   numUniformBuffers)` and build a pipeline (see `Renderer::init`).

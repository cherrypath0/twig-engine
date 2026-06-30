# Models: GLB loading & procedural meshes

Geometry lives in [`src/gltf/model.*`](../src/gltf/model.hpp). A mesh is loaded
on the CPU as `MeshData` (interleaved `Vertex { pos, normal, uv }` + 32-bit
indices) and uploaded to a `GpuMesh` (vertex + index GPU buffers).

## GLB / glTF

```cpp
Model mdl;
if (model::load_glb("assets/models/thing.glb", mdl)) {
    for (const Primitive& p : mdl.primitives) {
        int mesh = scene.add_mesh(model::upload(dev, p.mesh));
        // p.material_name and p.base_color come from the glTF material
    }
}
```

- Parsing uses **cgltf** (vendored, `libs/cgltf/cgltf.h`); the implementation is
  compiled once in `src/core/thirdparty.cpp`.
- Both `.glb` (binary, self-contained) and `.gltf` are accepted.
- Each glTF **primitive** becomes one `Primitive` (its own draw call / material).
- Node world transforms are **baked into the vertices**, so the imported mesh is
  already in world space.
- `POSITION`, `NORMAL` and `TEXCOORD_0` are read; missing normals/UVs default
  sensibly. The base-colour factor of the primitive's material is captured in
  `Primitive::base_color`.

The engine auto-loads the first `*.glb`/`*.gltf` it finds in `assets/models/`
at startup (see `Engine::try_load_glb`). Just drop a file there.

## Procedural primitives

For prototyping without assets:

```cpp
model::make_cube(0.5f);            // half-extent
model::make_plane(20.0f, 20.0f);   // half-extent, uv tiling
model::make_sphere(0.5f, 24);      // radius, segments
```

These are what the default scene's floor / boxes / spheres are built from, and
their dimensions are matched to the Jolt collision shapes the engine spawns.

# The `.tgmat` material format

A small text format inspired by Source 2's `.tgmat`. It is parsed by
`material::load_tgmat` in [`src/material/material.cpp`](../src/material/material.cpp).

## Syntax

```text
// line comments and /* block comments */ are allowed
Material            // the block name is ignored; anything before '{' works
{
    shader        "mesh"
    g_tColor      "assets/textures/crate.png"   // optional base-colour texture
    g_vColorTint  [0.85 0.52 0.24 1.0]          // rgba in [0,1]
    g_flMetalness 0.05
    g_flRoughness 0.45
    g_flAmbient   0.16
}
```

- Values may be quoted strings, bare numbers, or `[ ... ]` vectors.
- The parser is whitespace/newline insensitive and tolerant of unknown keys
  (they are logged and ignored in dev builds).

## Keys

| Key (Source 2 name) | Aliases                         | Type    | Default | Meaning                          |
|---------------------|---------------------------------|---------|---------|----------------------------------|
| `shader`            | —                               | string  | `mesh`  | shader program (`shaders/<id>.*`)|
| `g_tColor`          | `color`, `basecolor`, `albedo`  | path    | (none)  | base-colour texture (PNG/JPG/…)  |
| `g_vColorTint`      | `tint`, `color_tint`            | vec4    | `1 1 1 1` | multiplied colour tint         |
| `g_flMetalness`     | `metalness`, `metallic`         | float   | `0.0`   | metalness                        |
| `g_flRoughness`     | `roughness`                     | float   | `0.5`   | roughness (drives specular)      |
| `g_flAmbient`       | `ambient`                       | float   | `0.15`  | flat ambient term                |

## How it is used

1. `Engine::load_materials` loads each `assets/materials/*.tgmat` into a
   `Material` and calls `material::resolve_gpu`, which uploads `g_tColor` via
   `stb_image` (falling back to a 1×1 white texture when absent).
2. Each `Entity` stores a material **name**; the renderer looks it up and pushes
   the tint/metalness/roughness/ambient into the fragment uniform block
   (`MeshFragUBO`) and binds the texture. When no texture is present, the shader
   uses the flat tint only (`lightDir.w` toggles texturing).

To add a material: drop a new `.tgmat` in `assets/materials/`, then reference its
file-stem name from an entity (or extend `Engine::load_materials`).

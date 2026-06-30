# Physics (Jolt)

Rigid-body physics is provided by **Jolt Physics**, wrapped behind a small
façade so no Jolt types leak into the rest of the engine:
[`src/physics/physics.hpp`](../src/physics/physics.hpp).

```cpp
PhysicsWorld phys;
phys.init();
PhysicsBody floor = phys.add_box({20, 0.5, 20}, {0, -0.5, 0}, /*dynamic=*/false);
PhysicsBody box   = phys.add_box({0.5, 0.5, 0.5}, {0, 4, 0}, /*dynamic=*/true);
phys.optimize();                 // after the static bodies are added
...
phys.update(dt);                 // step the simulation
em::vec3 p; em::quat r;
phys.get_transform(box, p, r);   // feed back into the entity transform
```

`PhysicsBody` is an opaque handle; `Engine::sync_physics` copies each dynamic
body's transform into its `Entity` every frame.

## Build integration (no CMake)

Jolt is **compiled from source** by the Taskfile (`jolt` task) — every `Jolt/**/*.cpp` is
built into `build/libJolt.a` and cached:

```text
JOLT_CXXFLAGS := -std=c++17 -O2 -msse4.2 -mpopcnt -mfpmath=sse -DNDEBUG -ffp-contract=off
```

The engine and the Jolt library are compiled with matching arch flags
(`-msse4.2 -mpopcnt`) and **neither** defines `JPH_ENABLE_ASSERTS` /
`JPH_PROFILE_ENABLED` / `JPH_DOUBLE_PRECISION`, so their ABIs match. When
`libs/JoltPhysics/Jolt/Jolt.h` is present the Taskfile defines
`ENGINE_WITH_JOLT` and links the archive.

## The stub backend

If `ENGINE_WITH_JOLT` is **not** defined (e.g. the mingw Windows cross-build),
`physics.cpp` compiles a tiny stub that applies gravity + a floor plane so the
engine still runs. `PhysicsWorld::backend()` returns `"Jolt"` or `"stub"` and the
editor's Stats panel shows which is active.

## Layers

Two object layers (`NON_MOVING`, `MOVING`) and two broad-phase layers are set up
with the canonical Jolt filters: moving bodies collide with everything, static
bodies only with moving ones.

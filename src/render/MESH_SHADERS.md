# Mesh shading

Scene geometry can use task and mesh shaders through `VK_EXT_mesh_shader`.
The renderer also builds vertex pipelines and selects a path according to device
support, configuration, material, and geometry. See the
[renderer guide](README.md) for pass and shader locations.

## Geometry data

[`Meshlet.hpp`](../../include/Zahlen/Render/Meshlet.hpp) declares meshlet
partitioning and stream types. [`BuildMeshlets`](../engine/Meshlet.cpp) supplies
the shared implementation used to partition geometry. Meshlet streams contain
cluster metadata, vertex indices, and triangle micro-indices; the original
vertex/index streams remain available for vertex rendering and BLAS builds.

The task shader culls meshlets and emits work for the mesh shader. See
[`basic_task.slang`](../../resources/shaders/basic_task.slang) and
[`basic_mesh.slang`](../../resources/shaders/basic_mesh.slang). Partition limits,
task payload capacity, shader output limits, and micro-index alignment must
agree with the host declarations.

## Selection and pipelines

`EngineConfig::enableMeshShading` requests mesh shading at creation.
`ZHLN_NO_MESH_SHADING=1` forces the vertex path. The selection is fixed for the
render context's lifetime.

Device enablement requires extension support, task/mesh features, resolved
commands, and limits sufficient for the meshlet budget. Unsupported devices
use the vertex path.

Mesh pipelines use task/mesh stages instead of a vertex stage and omit
fixed-function vertex input and input assembly. Fragment interfaces differ
between G-buffer, shadow, and forward passes; update their geometry-output
variants together when changing varyings.

## Draw submission and culling

`DrawMeshTasks` dispatches task workgroups for a meshlet-bearing instance.
Indirect mesh-task commands have no `firstInstance` field, so the renderer's
instance-ID convention for vertex indirect draws cannot be reused unchanged.
Mesh shading uses per-draw recording with task-shader frustum and cone culling;
keep graph recording topology consistent with draw-path selection.

The shadow pass uses its cascade visibility list. Hi-Z occlusion culling is
disabled while mesh shading is active.

| Geometry | Path |
|---|---|
| Eligible static meshlet geometry | Task/mesh when enabled |
| GPU-skinned geometry | Vertex |
| Debug line lists | Vertex |
| CSG stencil draws with pipeline overrides | Vertex |
| Particles, decals, and post-processing | Vertex |

## Tests

[`TestMeshShaders.cpp`](../../tests/render/TestMeshShaders.cpp) covers
partitioning, procedural meshlet streams, configuration selection, and
mesh/vertex render parity. GPU mesh tests skip when the device lacks support.
For test invocation, see the [test guide](../../tests/README.md).

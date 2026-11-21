# Renderer

This directory assembles scene rendering: frame data, pass ordering, material
pipelines, lighting, post-processing, and presentation. Shader implementations
live in [`resources/shaders`](../../resources/shaders/SHADER.md). The
[Vulkan backend](../vulkan/README.md) provides device access, GPU resource
lifetimes, command recording, synchronization, and render-graph machinery.

## Code map

| Area | Start here |
|---|---|
| Renderer interface and persistent state | [RenderContext.hpp](RenderContext.hpp), [Renderer.hpp](Renderer.hpp) |
| Initialization | [RenderInit.cpp](RenderInit.cpp), [`init/`](init/) |
| Frame preparation and recording | [RenderFrame.cpp](RenderFrame.cpp), [RenderFrame.hpp](RenderFrame.hpp) |
| Scene graph assembly and execution | [RenderGraphBuild.hpp](RenderGraphBuild.hpp), [RenderGraphBuilder.cpp](RenderGraphBuilder.cpp) |
| Pass implementations | [`passes/`](passes/) |
| Pipeline construction | [`pipelines/`](pipelines/), [PipelineRegistry.cpp](PipelineRegistry.cpp) |
| Shadows and volumetric systems | [`features/`](features/) |
| Geometry, textures, and targets | [GeometryManager.hpp](GeometryManager.hpp), [TextureManager.hpp](TextureManager.hpp), [TargetManager.hpp](TargetManager.hpp) |
| Draw submission | [DrawQueueManager.hpp](DrawQueueManager.hpp), [RenderDrawCommands.cpp](RenderDrawCommands.cpp) |
| Output targets and presentation | [FrameDestinations.hpp](FrameDestinations.hpp), [PresentationSurface.hpp](PresentationSurface.hpp), [RenderPresentation.cpp](RenderPresentation.cpp) |
| UI | [`ui/`](ui/) |

## Frame and pass structure

`Renderer` holds persistent resources. `RenderFrame` identifies the frame slot
and exposes the resources and transient state used while recording that frame.
Keep frame-local recording state separate from resources that survive frames.

`BuildFrameGraph` in `RenderGraphBuild.hpp` assembles the scene pass pack.
`RenderGraphBuilder.cpp` selects the anti-aliasing variant, binds logical graph
resources to renderer targets, and executes it. Read these together when
changing pass dependencies or output selection.

The scene pass pack groups work into:

- Shadow rendering and G-buffer culling, base rendering, and resolve; background,
  decals, and viewmodel contributions.
- Hierarchical depth and ambient-occlusion preparation.
- Sun shadow masking, clustered lighting, radiance-cascade GI, and emission.
- Lighting mips, opaque/translucent reflections, scene-color copy, and forward
  rendering.
- Bloom, optional FXAA or SMAA, and tonemap/output blit.

This is a navigation map, not an unconditional list of dispatches: settings and
resource availability control work inside passes. The pass pack and its declared
resource dependencies define execution. For example, the sun-shadow mask must
be produced before clustered lighting reads it.

## Algorithms and shader entry points

Paths below are relative to [`resources/shaders/`](../../resources/shaders/).

| Work | Host code | Shader sources |
|---|---|---|
| Meshlet culling and hierarchical depth | `passes/culling/`, `passes/gbuffer/` | `culling.slang`, `hiz_generate.slang`, `basic_task.slang`, `basic_mesh.slang` |
| Material evaluation | `pipelines/DeferredPbrPipeline.cpp`, `passes/forward/` | `material_model.slang`, `pbr_helpers.slang`, `basic.slang` |
| Clustered lighting | `passes/lighting/ClusteredLightingPass.*` | `cluster_*.slang`, `lighting.slang` |
| Shadows | `features/ShadowRenderer.*`, `features/ShadowAtlas.*`, `passes/lighting/SunShadowMaskPass.*` | `shadow_atlas.slang`, `sun_shadow_mask.slang` |
| Ground-truth ambient occlusion | `GtaoDesc` in `passes/lighting/RayTracingPasses.hpp`, `passes/culling/HiZGeneratePass.*` | `ao_gtao.slang`, `gtao_surface.slang` |
| Radiance-cascade GI | `passes/lighting/RayTracingPasses.*`, `RtgiCompositePass.*` in the same directory | `rcgi_*.slang` |
| Reflections | `passes/lighting/ReflectionPass.*`, `pipelines/ReflectionPipeline.*` | `reflection.slang` |
| Environment lighting | `IBLProcessor.hpp` | `ibl_bake.slang`, `brdf_lut.slang` |
| Volumetric fog | `features/VolumetricFogSystem.*`, `passes/volumetric/` | `volumetric_*.slang` |
| Skinning and particles | `pipelines/ComputeSimPipeline.*`, `passes/simulation/` | `skinning.slang`, `particle_update.slang`, `mesh_particle_update.slang` |
| Bloom, anti-aliasing, and output | `passes/postprocess/`, `passes/aa/` | `bloom_*.slang`, `fxaa.slang`, `SMAA.slang`, `blit.slang` |

See the [shader guide](../../resources/shaders/SHADER.md) for material routing,
coordinate conventions, buffer access, and descriptor binding rules. Keep
algorithm equations and tuning rationale beside their shader implementations.

## CPU–shader data contract

`zshader` generates `ShaderBindings.hpp` and `GeneratedGpuTypes.hpp` from Slang
reflection. Change shader declarations rather than editing generated headers.

- [GpuAbi.hpp](GpuAbi.hpp) defines the host-facing scene payload limit and push
  layout; [GpuAbi.cpp](GpuAbi.cpp) exposes the generated layout.
- [GpuPack.hpp](GpuPack.hpp) and [GpuPack.cpp](GpuPack.cpp) pack host data into
  shader-facing representations.
- `Renderer::FrameHeapAddresses` in [RenderFrame.cpp](RenderFrame.cpp) selects
  the resources supplying generated frame-address fields.
- [PassPayloads.hpp](PassPayloads.hpp) and per-domain resource headers describe
  pass inputs. Resource binding and payload changes must agree with the shader
  declarations.

## Changing a rendering feature

1. Locate its pass and shader using the table above. Check the inputs, outputs,
   formats, and settings before changing the algorithm.
2. Update resource declarations and allocation when introducing targets or
   changing their formats. Wire the pass into the graph with its dependencies.
3. Update pipeline creation under `init/` or `pipelines/` as needed. Shader reload
   callbacks are registered in `init/RenderInitPostProcess.cpp` through
   [ShaderReloadRegistry](ShaderReloadRegistry.hpp); update registrations when
   changing watched shader paths.
4. Rebuild to regenerate shader bindings and compile host/shader layout checks.
5. Add or update a render regression following the
   [render-test guidance](../../tests/INVARIANT_TESTING.md). For visual comparison
   tooling, see the [fidelity harness](../../samples/FidelityHarness.md).

For Vulkan object ownership, layout transitions, heap writes, and submission
rules, use the [backend reference](../vulkan/README.md).

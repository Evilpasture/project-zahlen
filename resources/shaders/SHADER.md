# SHADER GUIDE

All shaders are Slang. The build passes `-matrix-layout-column-major` to
slangc; do NOT try to change matrix storage in source (no
`#pragma pack_matrix`). Matrix/vector math in source must match the C++
`JPH::Mat44` column-major layout exactly.

RIGHT HANDED COORDINATES, COLUMN MAJOR/VECTOR COLUMN, CCW ONLY.

Resource-free helpers live in `math/math.slang` (normalization and depth),
`math/transform.slang` (quaternions, tangents and normal mapping), and
`math/phase.slang` (volumetric scattering). Import them by their dotted module
names (`math.transform`, for example); the declaration inside each module is
its simple name (`module transform;`). They carry no descriptors or push data;
`pbr_helpers.slang` re-exports the math used by existing PBR consumers.

`material_model.slang` defines the compile-time `IMaterial` contract. Scene
G-buffer fragments specialize it for `ClearCoatMaterial` (base PBR plus optional
coat and anisotropy layers); forward and mesh-particle fragments use
`StandardPBRMaterial`. Shadow fragments sample only alpha. These structs are
shader-local, not a host material ABI or runtime material-type dispatch.
Forward transmission retains its separate refractive shading path.

`gpu_buffer.slang` supplies `GPUBuffer<T>` for typed loads, stores and uint
atomics through buffer device addresses. It is a non-owning, unchecked shader
view; callers must retain their existing zero-address and bounds guards.
Host-visible `uint64_t` addresses stay scalar in push data, instance data and
heap layouts, so this wrapper adds no descriptor or host ABI fields. Meshlets
and packed skin data still use word buffers with their existing byte strides.
The intentional invalid-pointer diagnostic in `hang_gpu.slang` stays raw.

Descriptor binding authority lives in the shaders: the C++ side reflects
the compiled SPIR-V (`ReflectedLayout`) instead of declaring static
layouts. Keep `GlobalSceneRegistry` member order stable in `common.slang` —
binding numbers follow declaration order, and `globalTextures[]` (runtime
array) must stay LAST.

GPU types, cluster math, vertex unpacking, particle state, descriptor-heap
push-data layout, and IBL / SMAA / BRDF LUT generation are also Slang-owned.
The host checks its C++ structs against the compiled `gpu_abi` SPIR-V at compile
time (`src/render/GpuAbi.hpp`, which the renderer compiles, plus
`Vk::PushConstantLayoutMatchesAll` in every dispatch that writes a payload) and
dispatches the bake / cluster-bounds compute kernels instead of re-authoring the
layouts or integrators in C++. One deliberately narrow exception is a prepared
float HDR panorama dominated by one compact emitter: the host integrates the
*smooth remainder* into diffuse SH with exact source-texel solid angles, and
stores the emitter's direction and cosine-convolved RGB in the otherwise-unused
`FrameUniforms.sh[0..5].w` lanes. `EvaluateSH` adds that emitter analytically,
avoiding the negative SH ringing caused by a point-like HDR sun. Ordinary HDRs,
procedural skies, the BRDF LUT and the specular cube remain GPU-baked. The
pre-filtered specular cube always uses the original, complete panorama.

`zshader --abi <gpu_abi.spv> --out-gpu-types ...` additionally reflects that
same module into the generated host structs (`GeneratedGpuTypes.hpp`, re-exported
by `src/render/GpuLayout.hpp`, which is renderer-internal -- nothing under
`include/` reaches it). Five of those structs are hand-written instead, in
`Zahlen/Render/RenderData.hpp`, and the header aliases them and asserts their
layout against the reflection: one definition per concept, no conversion between
the engine's copy and the shader's. A field added, removed, reordered
or retyped here re-emits the host side on the next build; a Slang kind without a
C++ spelling fails that build by name instead. Two exceptions: `GPUMeshlet` is still
hand-written -- its ABI is the raw word protocol in `fetchMeshlet`, which no
`std140`/`std430` declaration of consecutive `float3`s can spell -- and
`ClusterVolume`'s generated size (8) is its `StructuredBuffer` stride, not the
16-byte `ConstantBuffer`-wrapper rounding SPIR-V reports. Both are documented in
`tools/zshader/GpuTypes.cpp`.

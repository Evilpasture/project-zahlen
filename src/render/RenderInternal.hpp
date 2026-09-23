// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderInternal.hpp
#pragma once
#include "DestinationRegistry.hpp"
#include "Rendering.hpp"
#include "diagnostics/GPUDiagnostics.hpp" // GPUDiagnostics
#include "diagnostics/GpuProfiler.hpp"    // Profiler::GpuProfiler
#include "graph/RenderGraph.hpp"          // GraphImage
#include "pipeline/ComputePass.hpp"       // DynamicComputePass, FixedComputePass
#include "pipeline/FullscreenPass.hpp"      // FullscreenPass
// No shader catalog here on purpose: it is generated (ShaderBindings.hpp, see
// tools/zshader) and is data, not code every render source needs -- the translation
// units that name a set include it themselves. GpuAbi.hpp is the one shader-facing
// header included here, because the check against gpu_abi.slang belongs in the
// renderer's sources, not in the engine's public types header.

#include "TextureManager.hpp" // Private header
#include "DrawCommands.hpp"     // Private header: draw payloads and the frame queues
#include "DrawQueueManager.hpp" // Private header: the frame queues and their CPU sort
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Log.hpp>
#include "PresentationTarget.hpp" // PresentationTarget: src/window's private seam, on this target's include path
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include "GpuAbi.hpp"
#include "ui/UIRenderer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// Adapter for Vk::ParallelCommandRecorder on the engine task system. Lives here
// because the graph's fork executor and the pass factories both need it, and it must
// not pull the task system into src/vulkan.
struct TaskSystemScheduler {
    template <typename... Tasks>
    void Dispatch(Tasks&&... tasks) const {
        constexpr size_t numTasks = sizeof...(Tasks);
        if constexpr (numTasks == 0) {
            return;
        }

        std::array<TaskSystem::Task, numTasks> fiberTasks {};
        size_t                                  idx = 0;

        ((fiberTasks[idx] =
              TaskSystem::Task {
                  .func =
                      [](void* arg) {
                          using DecayedTask = std::decay_t<decltype(tasks)>;
                          auto* taskPtr     = static_cast<DecayedTask*>(arg);
                          (*taskPtr)();
                      },
                  .arg = const_cast<void*>(static_cast<const void*>(std::addressof(tasks)))
              },
          ++idx),
         ...);

        TaskSystem::Counter sync;
        TaskSystem::Dispatch({fiberTasks.data(), numTasks}, &sync);

        // Cooperative yield: the stack frame holding fiberTasks and tasks stays frozen
        // and valid in memory.
        TaskSystem::Wait(&sync);
    }
};

} // namespace ZHLN

namespace ZHLN::Vk {

struct IBLPayload {
    Image                    brdfLutImage;
    ImageView                brdfLutView;
    Image                    prefilteredImage;
    ImageView                prefilteredView;
    std::array<JPH::Vec4, 9> shCoeffs {};
    // VK_EXT_descriptor_heap: create infos for the heap image descriptors.
    VkImageViewCreateInfo brdfLutViewInfo {};
    VkImageViewCreateInfo prefilteredViewInfo {};
};

} // namespace ZHLN::Vk

namespace ZHLN {

void               ApplyImageDebugNames(RenderContext::Impl& impl) noexcept;
[[nodiscard]] bool CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept;

// Shader-blob recipe for a material's graphics pipelines. Internal: public callers go
// through RenderContext::CreateMaterial(MaterialDesc); this raw form exists only to
// compile the engine's built-in scene shaders.
struct PipelineDesc {
    // Every stage arrives as a descriptor built from a generated module
    // (<ShaderBindings.hpp>), so bytes and entry point travel together. Which geometry
    // module pairs with which fragment module is the variant's business (see
    // GetSceneShaders in RenderResources.cpp) -- mixing variants mismatches varyings.
    ZHLN_ShaderDesc vertexShader;
    ZHLN_ShaderDesc fragShader;

    // VK_EXT_mesh_shader: optional task/mesh stages. When the device supports mesh
    // shading and `meshShader` is set, the material gets a SECOND pipeline from
    // task+mesh+fragment; the vertex pipeline is always built too, so the renderer can
    // fall back per draw call (skinned meshes, no meshlet streams, no support).
    ZHLN_ShaderDesc taskShader;
    ZHLN_ShaderDesc meshShader;
    bool            doubleSided   = false;
    bool            alphaBlend    = false;
    bool            additiveBlend = false; // Support for emissive particles
    bool            isLineList    = false;
};

// Environment-Toggleable Render Diagnostics (Impl in RenderFrame.cpp), read once at
// startup to triage run-to-run nondeterminism without RenderDoc or GPU-AV:
//   ZHLN_NO_GPU_CULLING=1  Force the CPU culling policy in MainPass1/2.
//   ZHLN_FORK_SEQUENTIAL=1 Record a Vk::Fork's sub-passes in stream order instead of
//                          parallel secondaries: same barriers, resources and image,
//                          minus the scheduler round trip and vkCmdExecuteCommands.
namespace Diag {
[[nodiscard]] bool DisableGpuCulling() noexcept;
[[nodiscard]] bool ForkSequentialForced() noexcept;
} // namespace Diag

// GenerationalPool Template

template <typename T, size_t MaxObjects, typename HandleType = uint64_t>
class GenerationalPool {
  public:
    enum class Error : uint8_t {
        InvalidHandle = 1, // The handle was 0/Null
        StaleHandle,       // Generational mismatch (the resource was already destroyed)
        OutOfBoundsIndex,  // Index exceeds pool capacity
        NullResource       // Internal error: slot points to null pointer
    };

    GenerationalPool() {
        _freeIndices.reserve(MaxObjects);
        for (size_t i = 0; i < MaxObjects; ++i) {
            _freeIndices.push_back(MaxObjects - 1 - i);
        }
        _generations.fill(1); // Generations start at 1
    }

    ~GenerationalPool() {
        // Automatically sweeps and safely destroys all remaining active allocations on shutdown
        for (size_t i = 0; i < MaxObjects; ++i) {
            if (_pointers[i] != nullptr) {
                _pool.Destroy(_pointers[i]);
            }
        }
    }

    // Non-copyable, non-movable matching engine context lifetime
    GenerationalPool(const GenerationalPool&)                    = delete;
    auto operator=(const GenerationalPool&) -> GenerationalPool& = delete;

    template <typename... Args>
    HandleType Create(Args&&... args) {
        if (_freeIndices.empty()) [[unlikely]] {
            ZHLN::Log(
                "ERROR: GenerationalPool has exceeded its maximum capacity of {}! Returning "
                "invalid handle.",
                MaxObjects
            );
            return static_cast<HandleType>(0);
        }
        uint32_t index = _freeIndices.back();
        _freeIndices.pop_back();

        uint32_t gen     = _generations[index];
        _pointers[index] = _pool.Create(std::forward<Args>(args)...);

        uint64_t packed = (static_cast<uint64_t>(gen) << 32) | index;
        return static_cast<HandleType>(packed);
    }

    void Destroy(HandleType handle) {
        auto rawHandle = static_cast<uint64_t>(handle);
        auto index     = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        auto gen       = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects || _generations[index] != gen || _pointers[index] == nullptr) {
            return; // Safely ignore stale or invalid handles
        }

        _pool.Destroy(_pointers[index]);
        _pointers[index] = nullptr;
        _generations[index]++; // Increment generation to invalidate stale handles
        _freeIndices.push_back(index);
    }

    [[nodiscard]] auto Resolve(HandleType handle) const noexcept -> std::expected<T*, Error> {
        auto rawHandle = static_cast<uint64_t>(handle);
        if (rawHandle == 0) [[unlikely]] {
            return std::unexpected(Error::InvalidHandle);
        }

        auto index = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
        auto gen   = static_cast<uint32_t>(rawHandle >> 32);

        if (index >= MaxObjects) [[unlikely]] {
            return std::unexpected(Error::OutOfBoundsIndex);
        }
        if (_generations[index] != gen) [[unlikely]] {
            return std::unexpected(Error::StaleHandle);
        }
        if (_pointers[index] == nullptr) [[unlikely]] {
            return std::unexpected(Error::NullResource);
        }

        return _pointers[index];
    }

  private:
    ObjectPool<T, MaxObjects>        _pool;
    std::array<T*, MaxObjects>       _pointers {};
    std::array<uint32_t, MaxObjects> _generations {};
    ZHLN::Array<uint32_t>            _freeIndices;
};

static constexpr uint32_t kGpuCullingSentinel        = 0xFFFFFFFF;
static constexpr Color4   kClearColorNormalRoughness = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};

static constexpr uint32_t kParallelChunkSize            = 256;

// VK_EXT_descriptor_heap sizing. The resource heap holds, in order: the scene registry
// slots (from the kSceneStaticResourceSlots head budget); the bindless texture array as
// ONE offset-addressed region, so index N lives at the reservation's base + N and the
// boundary below it is not hand-kept; the frame partitions, one per parity, rewound by
// HeapManager::BeginFrame; and the immediate partition, rewound by BeginImmediate for
// out-of-frame bakes. The sampler heap is static only -- a sampler binding sits at a
// constant heap offset, so there is nothing per-frame to partition.
//
// These are slot budgets, not boundaries: what a head does not use stays unused.
static constexpr uint32_t kSceneStaticResourceSlots = 16;
static constexpr uint32_t kSceneStaticSamplerSlots  = 16;
// kGlobalTextureSlots and the three kFallback*TextureIndex constants live in
// TextureManager.hpp, next to the table they index; they reach this header
// through the TextureManager include above.
// Summed over every descriptor-heap pass of a frame from the reflected binding counts,
// the widest configuration is about 150 slots per viewport, and a multi-viewport frame
// re-records the post chain per viewport. 4096 is that with a wide margin, not a
// per-pass budget. It is arithmetic on the binding tables rather than an observed peak,
// so the dev-build tripwires settle it: BeginFrame logs past 75% full and
// AllocateTransientResourceRange asserts on overflow, instead of an undersized
// partition aliasing one pass's descriptors onto another's.
static constexpr uint32_t kFrameTransientResourceSlots     = 4096;
static constexpr uint32_t kImmediateTransientResourceSlots = 64;
// Pass samplers stay static: each sampler binding of a pass owns one permanent
// sampler-heap slot for the life of the device.
static constexpr uint32_t kPassStaticSamplerSlots = 64;

static constexpr Color4 kClearColorScene    = {.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f}; // G-Buffer background theme
static constexpr Color4 kClearColorVelocity = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
// Emission is additive in the lighting pass, so the cleared value has to be a
// true zero -- the scene clear colour would add a constant glow to the sky.
static constexpr Color4 kClearColorEmissive = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr float  kClearDepthValue    = 1.0f;

// --- Layouts and Types
static constexpr VkShaderStageFlags kCommonStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// Per-pass descriptor layouts. Layout authority lives in the compiled shaders: every
// pass layout is reflected (SPIRV-Reflect) from its Slang-produced SPIR-V rather than
// declared as a static C++ `DescriptorLayout<...>` list. The aliases below keep the
// historical pass names while pointing at that one implementation.
using GlobalSceneLayout           = Vk::ReflectedLayout;
using TAALayout                   = Vk::ReflectedLayout;
using FXAALayout                  = Vk::ReflectedLayout;
using MLAALayout                  = Vk::ReflectedLayout;
using SMAAEdgeLayout              = Vk::ReflectedLayout;
using SMAAWeightLayout            = Vk::ReflectedLayout;
using SMAABlendLayout             = Vk::ReflectedLayout;
using LightingLayout              = Vk::ReflectedLayout;
using ReflectionLayout            = Vk::ReflectedLayout;
using BlitLayout                  = Vk::ReflectedLayout;
using BloomThresholdCSLayout      = Vk::ReflectedLayout;
using KawaseCSLayout              = Vk::ReflectedLayout;
using VolumetricClearLayout       = Vk::ReflectedLayout;
using VolumetricFogInjectLayout   = Vk::ReflectedLayout;
using VolumetricLightInjectLayout = Vk::ReflectedLayout;
using VolumetricIntegrationLayout = Vk::ReflectedLayout;
using VolumetricTemporalLayout    = Vk::ReflectedLayout;
using CullingLayout               = Vk::ReflectedLayout;
using HiZGenerateLayout           = Vk::ReflectedLayout;
using ClusterCullingLayout        = Vk::ReflectedLayout;
using BakeLayout                  = Vk::ReflectedLayout;
using DecalLayout                 = Vk::ReflectedLayout;

using ActiveGBuffer = Vk::GBufferLayout<
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>, // Index 0: sceneColor
    Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>,           // Index 1: velocityBuffer
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>,          // Index 2: normalRoughnessBuffer
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>  // Index 3: emissiveBuffer
    >;

// Keep these enumerator names identical to the compile-time graph pass names:
// CompileTimeFrameGraph resolves them through reflection and injects timestamps, so a
// new graph pass needs no profiling code in its record lambda.
enum class Stage : uint8_t {
    MainPass1,
    HiZGenerate,
    ClusterCulling,
    MainPass2,
    MainShadow,
    ParticleUpdate,
    MeshParticleUpdate,
    VolumetricClear,
    VolumetricFogInject,
    VolumetricLightInject,
    VolumetricIntegrate,
    VolumetricTemporal,
    Lighting,
    Reflection,
    TransPrePass,
    TransReflection,
    Forward,
    BloomKawase,
    DecalPass,
    TAA,
    FXAA,
    MLAA,
    SmaaEdge,
    SmaaWeight,
    SmaaBlend,
    Blit,
    Viewmodel,
};

using FrameProfiler = Profiler::GpuProfiler<Stage>;

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

template <ShaderStage Stage>
struct ShaderStageSource {
    static constexpr ShaderStage  stage = Stage;
    const char*                   path;
    std::span<const std::uint8_t> fallback;
    // nullptr: the entry point is reflected out of the SPIR-V module, so
    // VSMain/PSMain/CSMain/Smaa* resolve without per-call-site bookkeeping.
    const char* entryPoint = nullptr;
};

using VertexStageSource   = ShaderStageSource<ShaderStage::Vertex>;
using FragmentStageSource = ShaderStageSource<ShaderStage::Fragment>;
using ComputeStageSource  = ShaderStageSource<ShaderStage::Compute>;

// The stage flag a slot stands for, so a module's own stage can be held against
// the slot it feeds.
[[nodiscard]] consteval auto StageFlagOf(ShaderStage stage) noexcept -> VkShaderStageFlagBits {
    switch (stage) {
        case ShaderStage::Vertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_ALL;
}

// The stage source of one generated module (ShaderBindings.hpp): the path a hot reload
// rereads, the cooked bytes, and the entry point, all read out of the module type. The
// stage is a template argument checked against the stage the module was compiled for,
// so a vertex module cannot be handed to a fragment slot.
template <ShaderStage Stage, Vk::ShaderProgram Module>
[[nodiscard]] auto MakeStageSource() noexcept -> ShaderStageSource<Stage> {
    static_assert(
        Vk::StageOf<Module>() == StageFlagOf(Stage),
        "a stage source names a module compiled for that stage (<ShaderBindings.hpp>)"
    );
    return {.path = Module::Path, .fallback = Module::Bytes(), .entryPoint = Module::EntryPoint};
}

static constexpr uint32_t kGpuCullingMaxInstances        = 8192;
static constexpr uint32_t kGpuCullingMaxBatches          = 256;
static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

struct WorkerCmdContext {
    std::array<Vk::CommandPool<Vk::QueueType::Graphics>, 2> pools;
    std::array<ZHLN::Atomic<uint32_t>, 2>                   cmdCount {};
};

template <VkImageLayout ColorL, VkImageLayout DepthL>
struct SceneResources {
    Vk::TypedImage<ColorL> sceneColor;
    Vk::TypedImage<ColorL> velocity;
    Vk::TypedImage<ColorL> normRough;
    Vk::TypedImage<ColorL> emissive;
    Vk::TypedImage<DepthL> depth;
};

namespace Resource {
}

// Frame Graph Resource Tags
// Hi-Z mip levels generated per frame. The culling consumer clamps its occlusion-test
// level to the deepest generated mip, so bounds smaller than one mip texel are tested
// against that level's conservative max depth.
inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor    = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity      = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough     = Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
// Emission is its own G-Buffer channel, not a term folded into SceneColor: the lighting
// pass multiplies SceneColor by incident light, so anything baked there disappears the
// moment a surface is unlit.
using Res_Emissive      = Vk::GraphImage<"Emissive", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Depth         = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_ShadowMap     = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas   = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Lighting      = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HdrSceneColor = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
// A-Trous ping-pong scratch for the HDR scene denoiser: same size/format as the scene
// color it filters, final iteration writes back into hdrSceneColor.
using Res_DenoiseA      = Vk::GraphImage<"DenoiseA", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_DenoiseB      = Vk::GraphImage<"DenoiseB", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
// Half-resolution composed RTR result for the VNDF roughness band; the scale divisor
// also opts the target into storage-image usage in RenderInitTargets, like the bloom
// cascades.
using Res_RtrHalf       = Vk::GraphImage<"RtrHalf", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
// Half-resolution GTAO occlusion for the AO-only GI modes: a single [0,1] channel, so
// R8. Lighting depth-weighted-upsamples it.
using Res_Ao            = Vk::GraphImage<"Ao", VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomThresh   = Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomDown1    = Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomDown2    = Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomDown3    = Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 16>;
using Res_BloomUp2      = Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomUp1      = Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomFinal    = Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_SmaaEdge      = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight    = Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain     = Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;
using Res_VoxelMedia    = Vk::GraphImage<"VoxelMedia", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelLight    = Vk::GraphImage<"VoxelLight", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelInt      = Vk::GraphImage<"VoxelInt", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelHist     = Vk::GraphImage<"VoxelHist", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 1, true>;
using Res_VoxelResolved = Vk::GraphImage<"VoxelResolved", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_TransNorm     = Vk::GraphImage<"TransNorm", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_TransDepth    = Vk::GraphImage<"TransDepth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_TransLighting = Vk::GraphImage<"TransLighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HiZ           = Vk::GraphImage<"HiZMap", VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace Vk

using Res_AccumCurr = Vk::GraphImage<"AccumCurr", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_AccumNext = Vk::GraphImage<"AccumNext", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;

struct RenderContext::Impl {
    struct RenderState {
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> initialState;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           finalColor;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           bloomFinal;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> resourcesForAA;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> aaResult;
    };

    struct GraphResources {
        Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> sceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>           velocityBuffer;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          normalRoughnessBuffer;
        Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> emissiveBuffer;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     lightingTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     hdrSceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseA;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseB;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtrHalf;
        Vk::RenderTarget<VK_FORMAT_R8_UNORM>                ao;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomThresholdTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown1;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown2;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown3;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomUp2;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomUp1;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomFinalTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomBlurTarget;
        Vk::RenderTarget<VK_FORMAT_R8G8_UNORM>              smaaEdgeTarget;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          smaaWeightTarget;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>              shadowMap;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>              shadowAtlas;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelMedia;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelLight;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelIntegrated;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelHistory;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelResolved;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          transNormalBuffer;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>      transDepthBuffer;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     transLightingTarget;
        Vk::MipmappedRenderTarget<VK_FORMAT_R32_SFLOAT>     hizMap;

        struct ReflectMetadata {
            Res_SceneColor    sceneColor;
            Res_Velocity      velocityBuffer;
            Res_NormRough     normalRoughnessBuffer;
            Res_Emissive      emissiveBuffer;
            Res_Lighting      lightingTarget;
            Res_HdrSceneColor hdrSceneColor;
            Res_DenoiseA      denoiseA;
            Res_DenoiseB      denoiseB;
            Res_RtrHalf       rtrHalf;
            Res_Ao            ao;
            Res_BloomThresh   bloomThresholdTarget;
            Res_BloomDown1    bloomDown1;
            Res_BloomDown2    bloomDown2;
            Res_BloomDown3    bloomDown3;
            Res_BloomUp2      bloomUp2;
            Res_BloomUp1      bloomUp1;
            Res_BloomFinal    bloomFinalTarget;
            Res_SmaaEdge      smaaEdgeTarget;
            Res_SmaaWeight    smaaWeightTarget;
            Res_ShadowAtlas   shadowAtlas;
            Res_VoxelMedia    voxelMedia;
            Res_VoxelLight    voxelLight;
            Res_VoxelInt      voxelIntegrated;
            Res_VoxelHist     voxelHistory;
            Res_VoxelResolved voxelResolved;
            Res_TransNorm     transNormalBuffer;
            Res_TransDepth    transDepthBuffer;
            Res_TransLighting transLightingTarget;
            Res_HiZ           hizMap;
        };
    };

    static constexpr uint32_t SHADOW_RES          = 2048;
    static constexpr uint32_t NUM_CASCADES        = 4;
    static constexpr uint32_t MAX_PUNCTUAL_LIGHTS = 4;

    static constexpr uint32_t kMaxLineVertices               = 500'000;
    static constexpr uint32_t kMaxDebugVertices              = 500'000;
    static constexpr uint32_t kGpuParticleCount              = 65'536;
    static constexpr uint32_t kGpuCullingMaxInstances        = 8'192;
    static constexpr uint32_t kGpuCullingMaxBatches          = 256;
    static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

    // What this renderer draws into, as the presentation seam: the concrete
    // target (a ZHLN::Window, a headless extent, a DRM connector) is named by
    // whoever created the context and never here. This is the only reason no
    // window-system header is reachable from this file.
    PresentationTarget&                         presentationTarget;
    String64                                     appName;
    Vk::Context                                  ctx;
    // Driver pipeline cache handed to every pipeline the renderer builds. Declared
    // directly after `ctx` so reverse-order destruction retires it before the device.
    Vk::PipelineCache                            pipelineCache;
    // Where the cache is read at init and flushed on teardown, from
    // RenderConfig::pipelineCachePath -- the engine decides runtime locations, this
    // layer obeys. Empty until then: PipelineCache reads an empty path as "no
    // persistence", so a renderer never invents a place to write.
    std::string                                  pipelineCachePath;
    Vk::Allocator                                allocator;
    // The primary window's presentation: surface, swapchain, the frame's sync and
    // command objects, the depth target. Borrowed by the primary window's registry
    // entry, which is why the registry does not own it.
    Vk::SwapchainPresenter                       presenter;
    // Fixed at RenderContext::Create time (see PresentationMode); read by
    // EndFrame to decide whether to hand the finished frame to HostBlit.
    PresentationMode                             presentationMode = PresentationMode::NativeSwapchain;
    Vk::CommandPools<2, Vk::QueueType::Compute>  computePools;
    Vk::StagingRingBuffer                        stagingRingBuffer;
    mutable Vk::StagingRingBuffer                transferRingBuffer;

    mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Compute, 8>  computeCmdRing;

    Vk::CommandBuffer<Vk::QueueType::Compute> current_compute_cmd;

    std::unique_ptr<Vk::StagingContext>    stagingContext;
    Vk::DeletionQueue                      deletionQueue;
    std::optional<Vk::ScopedDeletionQueue> activeQueueGuard;

    ZHLN::Array<WorkerCmdContext>                  workerCmds;
    DoubleBuffered<Vk::ParallelCommandRecorder<2>> parallelRecorder;

    GraphResources graphResources;

    // Fixed-function scene viewport rectangle (framebuffer pixels, top-left
    // origin). Width or height <= 1 means full frame. See RenderContext::SetViewport.
    uint32_t viewportX = 0;
    uint32_t viewportY = 0;
    uint32_t viewportW = 0;
    uint32_t viewportH = 0;

    // The scene viewport in effect: the stored rectangle clamped to the framebuffer, or
    // the full framebuffer when none is active. This is the renderer's working form
    // (VkViewport, 0..1 depth range); the public GetViewport maps it back onto the
    // API-neutral ViewportRect.
    [[nodiscard]] constexpr auto EffectiveViewport() const noexcept -> VkViewport {
        const auto& fb = graphResources.sceneColor.extent;
        if (viewportW <= 1 || viewportH <= 1) {
            return VkViewport {
                .x        = 0.0F,
                .y        = 0.0F,
                .width    = static_cast<float>(fb.width),
                .height   = static_cast<float>(fb.height),
                .minDepth = 0.0F,
                .maxDepth = 1.0F,
            };
        }
        const uint32_t x = std::min<uint32_t>(viewportX, fb.width);
        const uint32_t y = std::min<uint32_t>(viewportY, fb.height);
        const uint32_t w = std::min<uint32_t>(viewportW, fb.width - x);
        const uint32_t h = std::min<uint32_t>(viewportH, fb.height - y);
        return VkViewport {
            .x        = static_cast<float>(x),
            .y        = static_cast<float>(y),
            .width    = static_cast<float>(w),
            .height   = static_cast<float>(h),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
        };
    }

    // Bounded Substruct for Double-Buffered Resources (Reflection-Safe for Clangd)
    struct PerFrameResources {
        DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;
        DoubleBuffered<Vk::Buffer>                                      lineVbos;
        DoubleBuffered<VkDeviceAddress>                                 lineVboAddresses;
        DoubleBuffered<Vk::Buffer>                                      clusterGridBuffers;
        DoubleBuffered<Vk::Buffer>                                      lightIndexListBuffers;
        DoubleBuffered<Vk::Buffer>                                      globalCounterBuffers;
        DoubleBuffered<Vk::Buffer>                                      frameUniformBuffers;
        DoubleBuffered<Vk::Buffer>                                      lightStorageBuffers;
        DoubleBuffered<Vk::Buffer>                                      instanceDataBuffers;
        DoubleBuffered<Vk::Buffer>                                      indirectCommandsBuffers;
        DoubleBuffered<Vk::Buffer>                                      indirectCommandsBuffersPass2;
        DoubleBuffered<Vk::Buffer>                                      secondPassCandidatesBuffers;
        DoubleBuffered<Vk::Buffer>                                      secondPassCountBuffers;
        DoubleBuffered<Vk::Buffer>                                      shadowIndirectBuffers;
        DoubleBuffered<Vk::Buffer>                                      jointBuffers;
        DoubleBuffered<VkAccelerationStructureKHR>                      tlas;
        DoubleBuffered<Vk::Buffer>                                      tlasBuffer;
        DoubleBuffered<Vk::Buffer>                                      tlasScratchBuffer;
        DoubleBuffered<Vk::Buffer>                                      tlasInstanceBuffers;
        DoubleBuffered<BufferHandle>                                    debugMeshHandles;
        DoubleBuffered<Vk::Buffer>                                      fogVolumesBuffer;

        void FlipAll() noexcept {
            ZHLN::Reflect::ForEachField(*this, [](auto& field) { FlipObject(field); });
        }
    };

    PerFrameResources frames;

    Vk::Buffer clusterBoundsBuffer;
    Vk::Buffer morphDeltasBuffer;

    Vk::ReflectedLayout bindlessLayout;

    // VK_EXT_descriptor_heap state. The global scene registry (common.slang's
    // GlobalSceneRegistry block) no longer lives in a descriptor set: its bindings are
    // mapped onto the heaps at pipeline creation and its per-frame buffers are selected
    // through a push-data device-address block.
    Vk::HeapManager heapManager;
    // `GpuAbi::kScenePushLayout` is where the frame addresses and descriptor index sit
    // in the push-data blob: read out of the GPU ABI module's own bytes at compile
    // time (GpuAbi.hpp), never reflected at boot and never hand-copied into the RHI.

    Vk::HeapMappingBundle sceneHeapMappings;      // descriptorSet = 0 (GlobalSceneRegistry)
    Vk::HeapMappingBundle decalSceneHeapMappings; // descriptorSet = 1 (decal.slang's scene subset)
    Vk::HeapMappingBundle decalHeapMappings;      // descriptorSet = 0 (texDepth + pointSampler)

    // Per-pass heap binding tables (baked after each pass's layout reflection).
    Vk::HeapPassBindings hizHeapBindings;
    Vk::HeapPassBindings cullingHeapBindings;
    Vk::HeapPassBindings clusterBoundsHeapBindings;
    Vk::HeapPassBindings clusterCullingHeapBindings;
    Vk::HeapPassBindings bakeHeapBindings;
    Vk::HeapPassBindings volumetricClearHeapBindings;
    Vk::HeapPassBindings volumetricFogInjectHeapBindings;
    Vk::HeapPassBindings volumetricLightInjectHeapBindings;
    Vk::HeapPassBindings volumetricIntegrationHeapBindings;
    Vk::HeapPassBindings volumetricTemporalHeapBindings;

    // Sampler create infos written into static sampler-heap slots.
    VkSamplerCreateInfo shadowSamplerInfo {};
    VkSamplerCreateInfo defaultSamplerInfo {};
    VkSamplerCreateInfo pointSamplerInfo {};
    VkSamplerCreateInfo blueNoiseSamplerInfo {};

    // Static image create infos for views that are not plain RenderTargets.
    VkImageViewCreateInfo shadowAtlasCubeViewInfo {};
    VkImageViewCreateInfo shadowAtlas2DViewInfo {};
    VkImageViewCreateInfo ltcMatViewInfo {};
    VkImageViewCreateInfo ltcAmpViewInfo {};

    // Static heap slots (allocated once at init).
    Vk::SamplerHandle globalSamplerSlot;
    Vk::SamplerHandle clampSamplerSlot;
    Vk::SamplerHandle pointSamplerSlot;
    Vk::TextureHandle iblPrefilteredSlot;
    Vk::TextureHandle iblBrdfLutSlot;
    Vk::TextureHandle transLightingSlot;
    Vk::TextureHandle decalDepthSlot;
    // The first slot of the globalTextures[] region is the texture manager's:
    // it reserves the region itself (ReserveBindlessRegion) and hands the base
    // back to the heap-mapping builders through BindlessBaseSlot().

    VkPipelineLayout emptyPipelineLayout = VK_NULL_HANDLE; // Spec-required null layout for every descriptor-heap pipeline

    Vk::Sampler globalSampler;
    Vk::Sampler clampSampler;
    Vk::Sampler defaultSampler;
    Vk::Sampler pointSampler;

    // NEAREST + REPEAT sampler for the blue noise tile: repeat addressing is what lets
    // the shader scroll it in hardware, and nearest matters because filtering a noise
    // texture averages neighbouring texels toward the mean -- destroying exactly the
    // high-frequency content it exists to supply.
    Vk::Sampler blueNoiseSampler;

    Vk::Image      volumetricNoiseImage;
    Vk::ImageView  volumetricNoiseView;
    VkImageViewCreateInfo volumetricNoiseViewInfo {};

    // The bindless slot arrays used to live here. They are the texture
    // manager's now, along with the free list and the pending-release queues:
    // reach them through textureManager.Image(slot) / .View(slot).

    Vk::FullscreenPass<TAALayout>        taaPass;
    Vk::FullscreenPass<FXAALayout>       fxaaPass;
    Vk::FullscreenPass<MLAALayout>       mlaaPass;
    Vk::FullscreenPass<SMAAEdgeLayout>   smaaEdgePass;
    Vk::FullscreenPass<SMAAWeightLayout> smaaWeightPass;
    Vk::FullscreenPass<SMAABlendLayout>  smaaBlendPass;

    Vk::FullscreenPass<LightingLayout>   lightingPass;
    Vk::FullscreenPass<ReflectionLayout> reflectionPass;
    Vk::FullscreenPass<ReflectionLayout> translucentReflectionPass;
    Vk::FullscreenPass<BlitLayout>       blitPass;

    // Dual Kawase bloom: one compute chain (threshold -> down x3 -> up x3) inside a
    // single frame-graph pass instead of seven raster passes.
    Vk::DynamicComputePass bloomThresholdCS;
    Vk::DynamicComputePass hdrDenoiseCS;
    Vk::DynamicComputePass rtrHalfCS;
    Vk::DynamicComputePass gtaoCS;
    Vk::DynamicComputePass bloomDownCS;
    Vk::DynamicComputePass bloomUpCS;
    Vk::HeapPassBindings bloomThresholdHeapBindings;
    Vk::HeapPassBindings hdrDenoiseHeapBindings;
    Vk::HeapPassBindings rtrHalfHeapBindings;
    Vk::HeapPassBindings gtaoHeapBindings;
    Vk::HeapPassBindings bloomDownHeapBindings;
    Vk::HeapPassBindings bloomUpHeapBindings;

    Vk::FixedComputePass clusterBoundsPass;
    Vk::FixedComputePass clusterCullingPass;
    Vk::DynamicComputePass cullingPass;
    Vk::DynamicComputePass skinningPass;
    Vk::DynamicComputePass proceduralBakePass;
    Vk::DynamicComputePass hangGpuPass;
    Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout> volumetricClearPass;
    Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout> volumetricFogInjectPass;
    Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout> volumetricLightInjectPass;
    Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout> volumetricIntegrationPass;
    Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout> volumetricTemporalPass;

    Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMapPrev;
    ZHLN::Array<Vk::ImageView>             shadowCascadeViewsPrev;

    Vk::PipelineLayout skinningPipelineLayout;
    VkPipelineLayout   shadowPipelineLayout         = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout
    VkPipelineLayout   punctualShadowPipelineLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout

    Vk::TypedPipeline<0, true> shadowPipeline;
    Vk::TypedPipeline<0, true> punctualShadowPipeline;

    // VK_EXT_mesh_shader twin of `shadowPipeline` (basic_task + basic_mesh + PSShadow);
    // invalid when the device cannot mesh-shade, and the cascade loop then keeps issuing
    // the indirect vertex draws.
    Vk::TypedPipeline<0, true> shadowMeshPipeline;

    // Create-time request (RenderConfig::enableMeshShading, AND-ed with the
    // ZHLN_NO_MESH_SHADING env latch at Create). Device support is separate.
    bool enableMeshShading = true;

    // True when the meshlet path should be used for scene geometry this frame.
    [[nodiscard]] bool MeshShadingActive() const noexcept {
        return enableMeshShading && ctx.MeshShadersSupported();
    }

    // The optional mesh-shader features (multiviewMeshShader for SV_ViewID in
    // the task/mesh stages, meshShaderQueries for the pipeline-statistic bits)
    // are device-creation state, so the RHI answers for them: ask
    // ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>(...). Nothing here
    // keeps a copy -- there is no per-feature flag on Impl to fall out of sync.

    // The bindless texture table: globalTextures[], the image and view behind
    // each slot, and the handle -> slot records. Constructed in Impl's
    // initializer list from the members declared above it, so it borrows the
    // device, the allocator, the staging ring, the graphics command ring and
    // the heap manager rather than reaching back through RenderContext.
    TextureManager textureManager;

    Vk::Buffer                  particleBuffer;
    Vk::DynamicComputePass particleUpdatePass;
    VkPipelineLayout            particleRenderLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout
    Vk::TypedPipeline<1, false> particleRenderPipeline;

    Vk::DynamicComputePass meshParticleUpdatePass;
    VkPipelineLayout meshParticleRenderLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout
    Vk::Pipeline     meshParticleRenderPipeline;
    Vk::Pipeline     meshParticleShadowPipeline;

    Vk::ReflectedLayout decalDescLayout;                      // Reflection only: decal bindings map onto the heaps
    VkPipelineLayout         decalPipelineLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout
    Vk::Pipeline             decalPipeline;

    VkPipelineLayout linePipelineLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout
    Vk::Pipeline     linePipeline;
    uint32_t         activeLineVertexCount = 0;
    uint32_t         lineInstanceId        = 0;

    std::expected<void, ErrorCode> BuildLinePipeline();
    std::expected<void, ErrorCode> InitLineBuffers() noexcept;
    std::expected<void, ErrorCode> AllocateDynamicVertexBuffers(
        size_t                           maxVertices,
        DoubleBuffered<Vk::Buffer>&      bufs,
        DoubleBuffered<VkDeviceAddress>& addrs,
        const char*                      label,
        Vk::BufferUsage                  extraFlags = Vk::BufferUsage::None
    ) noexcept;
    void FlushLineQueue();

    // --- VK_EXT_descriptor_heap frame bookkeeping
    // Device addresses of the current frame's scene buffers, in
    // GlobalSceneRegistry order {frame, lights, instances, joints, prevJoints, morphDeltas}.
    [[nodiscard]] auto FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount>;
    // Binds both heaps and pushes the frame addresses at their reflected offsets.
    // Heap-using segments call this first: legacy set/push-constant commands elsewhere
    // in the frame invalidate heap and push-data state, so every segment re-establishes it.
    void BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept;

    // --- VK_EXT_descriptor_heap init
    // Creates the heaps, allocates the static slots, and bakes the
    // VkDescriptorSetAndBindingMappingEXT tables used at pipeline creation.
    std::expected<void, ErrorCode> InitSceneHeaps(const VkSamplerCreateInfo& globalSamplerInfo, const VkSamplerCreateInfo& clampSamplerInfo) noexcept;
    void                       BuildSceneHeapMappings() noexcept;
    void                       BuildDecalHeapMappings() noexcept;
    void                       WriteSceneStaticImageDescriptors() noexcept;
    void                       WritePointSamplerToHeap(const VkSamplerCreateInfo& info) noexcept;
    void                       WriteTransLightingToHeap() noexcept;
    void                       InitPassSamplerDescriptors() noexcept;
    [[nodiscard]] std::expected<void, ErrorCode> InitBakeHeapBindings() noexcept;
    // The globalTextures[] slot table -- adopt / release / reclaim, the heap
    // write and the slot arrays -- lives on `textureManager`.
    // `Declared` is the shader set the bake block serves, passed by the caller so this
    // header stays free of the catalog (see the include note above). The modules are the
    // ones whose dispatch reads the payload.
    template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
    [[nodiscard]] auto BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
        -> std::expected<uint32_t, ErrorCode>;

    Vk::ReflectedLayout cullingLayout; // Reflection only: drives the heap binding table
    Vk::DynamicComputePass hizGeneratePass;
    Vk::ReflectedLayout hizDescLayout; // Reflection only

    Vk::ReflectedLayout bloomThresholdCSLayout; // Reflection only
    Vk::ReflectedLayout hdrDenoiseCSLayout;     // Reflection only
    Vk::ReflectedLayout rtrHalfCSLayout;        // Reflection only
    Vk::ReflectedLayout gtaoCSLayout;           // Reflection only
    Vk::ReflectedLayout bloomDownCSLayout;      // Reflection only
    Vk::ReflectedLayout bloomUpCSLayout;        // Reflection only

    Vk::ReflectedLayout clusterCullingDescLayout; // Reflection only
    Vk::ReflectedLayout clusterBoundsDescLayout;  // Reflection only

    Vk::ReflectedLayout proceduralBakeDescLayout; // Reflection only

    ZHLN::Array<Vk::ImageView> shadowCascadeViews;
    Vk::ImageView              shadowAtlasCubeView;
    Vk::ImageView              shadowAtlas2DView;
    ZHLN::Array<Vk::ImageView> punctualShadowViews;
    Vk::Sampler                shadowSampler;

    Vk::Image     ltcMatImage;
    Vk::ImageView ltcMatView;
    Vk::Image     ltcAmpImage;
    Vk::ImageView ltcAmpView;

    Vk::IBLPayload iblPayload;

    GenerationalPool<NativeMesh, 8192, BufferHandle>       meshPool;
    GenerationalPool<NativeMaterial, 2048, PipelineHandle> materialPool;

    ZHLN::HashMap<AssetID, Mesh>          assetMeshMap;
    ZHLN::HashMap<MaterialID, Material>   assetMaterialMap;
    ZHLN::HashMap<uint64_t, BufferHandle> skinnedScratchMap;
    // Cache-key -> {packed ECS owner, buffer}; owner survives component erasure
    // so RenderContext can reconcile the allocation without callbacks.
    ZHLN::HashMap<uint64_t, ZHLN::Pair<uint64_t, BufferHandle>> particleBufferMap;

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> tracked2DEmitters;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> tracked3DEmitters;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> trackedEntityBuffers;

    // The frame's draw submission and the CPU sort that orders it. Was a bare
    // RenderQueues plus three sort scratch arrays and a SortDrawQueue method on
    // Impl; the scratch and the algorithm are the manager's now.
    DrawQueueManager   queues;
    ZHLN::Array<Light> mappedLights;

    // Live entry count of the light storage buffer -- what SetLights last clamped and
    // wrote. SetFrameData stamps it into every uploaded FrameUniforms::lightCount, so
    // the shader's light-index bound and the buffer it indexes cannot disagree.
    uint32_t packedLightCount = 0;

    // What the frame's scene passes actually did, stamped while they ran. `RenderQueues`
    // is cleared by EndFrame and the indirect command arrays have one writer (the GPU
    // instance-culling path, which mesh shading replaces), so by the time telemetry
    // looks neither can say whether the frame commanded geometry. These stamps survive
    // EndFrame.
    struct ScenePassStamp {
        uint32_t draws         = 0; // draws the queue held when the pass ran
        uint32_t csgDraws      = 0;
        uint32_t meshParticles = 0;
        uint32_t shadowDraws   = 0; // cascade + punctual instances the shadow pass wrote
        bool     ran           = false;
        bool     gpuCulling    = false; // the indirect instance-culling path drove this pass
        bool     meshShading   = false;
    };
    ScenePassStamp scenePass1;
    ScenePassStamp scenePass2;
    ScenePassStamp shadowPass;

    Vk::Pipeline     csgWritePipeline;
    Vk::Pipeline     csgDifferencePipeline;
    Vk::Pipeline     csgIntersectionPipeline;
    VkPipelineLayout csgPipelineLayout = VK_NULL_HANDLE; // Raw alias of the spec-required null heap layout

    UIRenderer uiRenderer;

    // Destinations: windows and offscreen render textures. A destination is a
    // subresource, never a mode; `RenderAttachment` is its only public name and this
    // registry turns it back into a concrete VkImage/ImageView. Swapchain images are
    // non-owning (they die with the swapchain); render textures are owned by the texture
    // heap and only referenced here.

    // Every destination this context knows about: the windows it presents,
    // and the records their images are addressed through.
    DestinationRegistry destinations;

    // Records the sub-passes of a `Vk::Fork` concurrently: the graph computes the
    // barriers for the union of their usages, hands the bodies here, and this replays the
    // secondaries with vkCmdExecuteCommands. No base class -- the graph takes the
    // executor as a template parameter, so the call resolves statically.
    struct ForkReplayer {
        explicit ForkReplayer(RenderContext::Impl& self) noexcept: impl(&self) {
        }
        RenderContext::Impl* impl;
        [[nodiscard]] auto ForkSecondariesActive() const noexcept -> bool {
            return impl->frameState.inForkSecondary;
        }
        void ExecuteFork(VkCommandBuffer cmd, std::span<const Vk::ForkBody> bodies) noexcept;
    };
    static_assert(Vk::ForkRecorder<ForkReplayer>);
    std::unique_ptr<ForkReplayer> forkReplayer;

    // The frame's own bookkeeping: what this frame did, and what it therefore owes
    // the next one. Every flag here is a renderer decision -- skinning ran, compute
    // was submitted, a sub-pass is recording into a secondary, the extent changed,
    // the clustered bounds are stale. None of them is a device fact, which is why
    // none of them lives in the RHI: src/vulkan has no concept of a skinning pass or
    // of a frame's compute-to-graphics ordering to hang them on.
    //
    // Bundled because BeginFrame and EndFrame both clear the same three of them;
    // as loose members that was a list to keep in sync at every reset site.
    struct FrameTransientState {
        // True once DispatchCompute has submitted this frame's compute work, so the
        // graphics submit knows whether waiting on the compute timeline is
        // meaningful -- a frame that never dispatched must not wait on a value
        // nothing signals.
        bool computeSubmitted = false;

        // True while a forked sub-pass body records into a SECONDARY buffer inheriting
        // the primary's heap bindings: such a body must not rebind the heaps (see
        // FrameRecorder::heapsInherited); the address block was already re-pushed.
        bool inForkSecondary = false;

        // True once a draw this frame used a skinned scratch VBO, so the skinning
        // dispatch and its barriers are only recorded when something needs them.
        bool hasSkinned = false;

        // The two below deliberately survive Reset(): they are set by a window or
        // camera event and consumed later, on a frame boundary of their own.
        // Clearing them per frame would drop a resize that arrived mid-frame.
        bool resized = true;
        // FOV or viewport aspect changed, so the clustered bounds dispatch has to
        // re-run; cleared when it does (see RecordComputeFrame).
        bool clusterBoundsDirty = true;

        // The per-frame flags. Not the latched pair above: those are cleared by
        // whoever consumed them, not by the frame boundary. inForkSecondary is
        // already false by the time either boundary runs (ExecuteFork restores
        // it before returning), so clearing it here is a net, not a state
        // change.
        void Reset() noexcept {
            computeSubmitted = false;
            inForkSecondary  = false;
            hasSkinned       = false;
        }
    };

    FrameTransientState frameState;

    // The executor to hand `CompileTimeFrameGraph::Execute`; its concrete type is what
    // the graph's `ForkPolicyT` deduces to.
    [[nodiscard]] auto ForkExecutor() noexcept -> ForkReplayer* {
        return forkReplayer.get();
    }
    // Heap-inheritance mode for a sub-pass body, so the same lambda works standalone on
    // the primary or replayed as a forked secondary.
    [[nodiscard]] auto InheritsHeaps() const noexcept -> bool {
        return frameState.inForkSecondary;
    }

    // --- Destinations
    // Three files, three jobs: RenderDestinations.cpp adapts a Window to a
    // Vk::SwapchainPresenter and vends its attachment, RenderTexture.cpp owns the
    // offscreen targets, RenderPresentation.cpp closes the frame.

    // A destination lookup's answer: the window's entry, and whether this call created
    // it. The lookup reports what happened; the boundary that owns the policy decides
    // what to say about it.
    struct DestinationVend {
        DestinationRegistry::WindowEntry* entry   = nullptr;
        bool                              created = false;
    };

    // Creates a window's entry when it has none -- for a caller-owned window, its own
    // surface and presenter. Every failure leaves through the error slot: a
    // DestinationError for this layer's decisions, the RHI's or the window's own code
    // when the failure was theirs to describe.
    [[nodiscard]] auto FindOrCreateDestination(PresentationTarget& aux, bool primary) noexcept
        -> std::expected<DestinationVend, ErrorCode>;
    // Acquires the frame's image through the window's presenter and makes sure a record
    // points at it: the handle it was vended as, std::nullopt when the window cannot
    // present this frame (not an error), the presenter's code when the acquire failed.
    // Deliberately does not touch the command buffer -- opening it is AcquireTarget's.
    [[nodiscard]] auto AcquireDestinationImage(DestinationRegistry::WindowEntry& dest) noexcept
        -> std::expected<std::optional<DestinationRegistry::Handle>, ErrorCode>;
    // Closes one destination for presentation and answers what the frame has for it: the
    // receipt a pass left, a receipt for the background the frame fills in when no pass
    // wrote the image, or the reason it will not be presented.
    //
    // Called by the presentation loop one destination at a time, not as a sweep before
    // presenting: a destination is closed by the same step that decides whether to show
    // it, so an acquired image is never neither written nor accounted for.
    [[nodiscard]] auto ReconcileDestination(DestinationRegistry::WindowEntry& dest) noexcept
        -> FrameOutcome<DestinationRegistry::Rendered>;
    // The attachment this frame already has for a window, and nothing else.
    // A query in the strict sense: no acquire, no fence wait, no command
    // buffer, no state a later call could notice as changed.
    [[nodiscard]] auto TargetAttachment(const PresentationTarget& aux) noexcept -> std::optional<RenderAttachment>;
    // The frame verb behind RenderContext::AcquireTarget: creates the window's
    // destination when it has none, acquires its image and opens its recording. Returns
    // the attachment, std::nullopt when there is nothing to draw into, else the reason in
    // the error slot.
    [[nodiscard]] auto AcquireTarget(const PresentationTarget& aux) noexcept -> FrameOutcome<RenderAttachment>;
    // The stream a pass records a target through: the destination owning the record and
    // that destination's recording for this frame. Null when it has none open, which is a
    // pass with nothing to record into. A lookup of existing frame state; it starts
    // nothing.
    [[nodiscard]] auto RecordingFor(const DestinationRegistry::Record& record) const noexcept -> VkCommandBuffer;
    // The frame's own stream: the destination the frame is drawing into. What a
    // pass with no destination of its own falls back to, and where diagnostics
    // write.
    [[nodiscard]] auto FrameCommand() const noexcept -> VkCommandBuffer;
    void               ReleaseTarget(const PresentationTarget& aux) noexcept;
    void               DestroyDestinations() noexcept;
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;

    // Presents every window that received draw commands this frame: names the
    // waits the frame's other queues impose, hands the destination to its
    // presenter, and recovers from a present that did not happen.
    [[nodiscard]] auto PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal>;

    // Scene state uploaded once per RenderScene call (queue sort, instance data,
    // skinning, TLAS); the graph itself is recorded by DeferredPbrPipeline.cpp.
    // Destination the scene's output goes to, resolved from the caller's SceneView.
    // Copied by value: the registry grows, so a pointer into it would not stay valid.
    std::optional<DestinationRegistry::Record> sceneTarget;

    // Presenter of the window whose attachment is being rendered into, the primary's own
    // while nothing has been vended. Reads here are about the frame's *targets* -- above
    // all the depth buffer, which ping-pongs with the window that owns it.
    [[nodiscard]] auto ActivePresentation() noexcept -> Vk::SwapchainPresenter& {
        if (const PresentationTarget* active = destinations.ActiveTarget(); active != nullptr) {
            if (auto* dest = destinations.Find(*active); dest != nullptr) {
                return dest->Presenter();
            }
        }
        return presenter;
    }

    // Adopts the optics of one view: matrices, camera position and time are per-view,
    // unlike the sun/sky/probe state SetFrameData owns.
    void ApplySceneView(const SceneView& view) noexcept;

    // Scene state uploaded once per RenderScene call (queue sort, instance
    // data, skinning, TLAS). The graph itself is recorded by
    // src/render/pipelines/DeferredPbrPipeline.cpp.
    void PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept;


    Vk::RayTracingContext rtCtx;

    JPH::Mat44    current_view_proj    = JPH::Mat44::sIdentity();
    JPH::Mat44    unjittered_view_proj = JPH::Mat44::sIdentity();
    JPH::Mat44    shadowProjView       = JPH::Mat44::sIdentity();
    FrameUniforms currentUniforms {};
    float         currentDt = 0.0166f;

    // Canonical graphics configuration: the single renderer-side source of truth, written
    // only through ApplySettings and never queried from ECS components inside the
    // renderer.
    GraphicsSettings settings {};

    FrameProfiler      gpuProfiler;
    Vk::GPUDiagnostics gpuDiagnostics;

    // Pipeline statistics from completed frames (added during BeginFrame retrieval,
    // drained by PipelineStatsCapture::Consume); render/test thread only, like the
    // profiler retrieval.
    GpuPipelineCounters pendingPipelineCounters {};

    struct ShaderReloadRegistration {
        std::string              name;
        std::vector<std::string> paths;
        std::function<void()>    reloadCallback;
    };

    // The watcher belongs to Engine; renderer ownership is limited to its directory
    // subscription and the path-to-pipeline callback registry.
    FS::FileSystemWatcher*                     fileSystemWatcher = nullptr;
    FS::FileWatchHandle                        shaderDirectoryWatch = 0;
    std::vector<ShaderReloadRegistration> shaderReloads;

    // The globalTextures[] slot bookkeeping -- the high-water mark, the free
    // list and the per-parity pending-release queues -- moved to
    // TextureManager, which owns the slot arrays those queues hand back to.

    uint32_t nextMorphDeltaIndex = 0;
    uint32_t smaaAreaTexIdx      = 0;
    uint32_t smaaSearchTexIdx    = 0;
    // Bindless index of the blue noise tile (resources/shaders/LDR_RGBA_0.png) and its
    // extent, needed to build the per-pass TypedImage descriptor.
    uint32_t blueNoiseTexIdx     = 0;
    uint32_t blueNoiseWidth      = 0;
    uint32_t blueNoiseHeight     = 0;
    // Kept as a member (like iblPayload's view infos) because TypedImage holds a pointer
    // to it and the reflection pass builds its descriptor inline.
    VkImageViewCreateInfo blueNoiseViewInfo {};

    // The view parameters the clustered bounds dispatch was last run with: the
    // comparison in SetFrameData is what sets frameState.clusterBoundsDirty.
    float lastAspectRatio = 0.0f;
    float lastFov         = 0.0f;

    ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const noexcept {
        gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
    void RegisterShader(const ZHLN_ShaderDesc& desc, std::string_view fallbackEntry = "main") const noexcept {
        gpuDiagnostics.RegisterShader(desc, fallbackEntry);
    }

    Impl(PresentationTarget& target, FS::FileSystemWatcher* watcher)
        : presentationTarget(target),
          // Plain construction-order injection: every dependency below is
          // declared above `textureManager`, so the manager borrows the device
          // context, the allocator, the staging ring, the graphics command ring
          // and the heap manager and never reaches back through RenderContext.
          // The bindless region it addresses inside the heap is a product of
          // InitSceneHeaps, so that arrives through ReserveBindlessRegion.
          textureManager(ctx, allocator, stagingRingBuffer, graphicsCmdRing, heapManager),
          fileSystemWatcher(watcher) {}

    ~Impl() {
        // Destinations own per-window swapchains and their render targets; both must go
        // before the device does. ReleaseTarget is the per-window path, this the teardown.
        DestroyDestinations();
        if (fileSystemWatcher != nullptr && shaderDirectoryWatch != 0) {
            static_cast<void>(fileSystemWatcher->Unwatch(shaderDirectoryWatch));
        }
        graphicsCmdRing.Cleanup();
        transferCmdRing.Cleanup();
        if (ctx.Device() != VK_NULL_HANDLE) {
            for (uint32_t i = 0; i < 2; ++i) {
                if (frames.tlas[i] != VK_NULL_HANDLE) {
                    rtCtx.DestroyAccelerationStructure(frames.tlas[i]);
                }
            }
        }
    }

    [[nodiscard]] std::expected<void, ErrorCode> InitSubsystems(const RenderConfig& cfg, int width, int height);
    [[nodiscard]] std::expected<void, ErrorCode> InitDiagnosticsAndProfiling();
    [[nodiscard]] std::expected<void, ErrorCode> InitCorePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> InitParallelRecorders();
    [[nodiscard]] std::expected<void, ErrorCode> BuildSpecializedLightingPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BuildVolumetricPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BakeSMAALUTs();

    struct alignas(16) ComputePushConstants {
        VkDeviceAddress       particleBufferAddr;
        uint32_t              particleCount;
        float                 deltaTime;
        ParticleEmitterParams p;
    };

    struct CullingConstants {
        JPH::Mat44 viewProj;
        float      hizScreenSize[2];
        uint32_t   maxHiZMipLevel;
        uint32_t   drawCount;
        uint32_t   passIndex;
    };

    struct ParticleRenderPushConstants {
        VkDeviceAddress particleBufferAddr;
        uint32_t        alignment;
        uint32_t        textureIndex;
    };

    struct alignas(16) MeshParticleComputePush {
        VkDeviceAddress           particleBufferAddr;
        uint32_t                  particleCount;
        float                     deltaTime;
        MeshParticleEmitterParams p;
    };

    struct MeshParticleRenderPush {
        VkDeviceAddress particleBufferAddr;
        VkDeviceAddress posAddress;
        VkDeviceAddress attrAddress;
        VkDeviceAddress iboAddress;

        float baseColorFactor[4];
        float emissiveFactor[4];

        uint32_t indexCount;
        uint32_t albedoIdx;
        uint32_t normalIdx;
        uint32_t pbrIdx;
        uint32_t emissiveIdx;
        float    roughness;
        float    metallic;
        float    alphaCutoff;
        uint32_t alphaMode;

        // The shader declares this word `_padding`: the cascade comes from ViewIndex, so
        // nothing reads what the host wrote. Named as the module names it so the two
        // structs compare member for member (GpuAbi.hpp).
        uint32_t _padding;
    };
    static_assert(sizeof(MeshParticleRenderPush) == 104);

    // Push blocks shared by more than one pass. These belong here, not in a public
    // header: what a pipeline pushes is this layer's interface with its shaders. The shader declarations live in the modules that read them, and each struct
    // below is held against those modules where it is pushed -- the dispatch, execute and
    // draw entry points require the module name and assert Vk::PushConstantLayoutMatchesAll.

    // The per-draw block every scene pipeline's vertex (or task) stage reads:
    // basic.slang and basic_task.slang both build on common.slang's
    // declaration.
    struct ObjectConstants {
        uint32_t instanceId;
        uint32_t isShadowPass;
    };
    static_assert(sizeof(ObjectConstants) == 8);

    // ui.slang's block: one batch's place in the UI vertex pool, by address.
    struct UIObjectConstants {
        JPH::Mat44 orthoMatrix;
        uint64_t   posAddress;
        uint64_t   attrAddress;
        uint32_t   albedoIdx;
        uint32_t   isSDF;
        uint32_t   useTextureColor;
    };
    static_assert(sizeof(UIObjectConstants) == 96);

    struct alignas(16) VolumetricFogPushConstants {
        float density;
        float heightFalloff;
        float heightOffset;
        float anisotropy;

        float scatteringColor[3];
        float noiseScale;

        float absorptionColor[3];
        float noiseSpeed;

        float emissiveColor[3];
        float noiseIntensity;

        uint32_t volumeCount;
        uint32_t enableNoise;
        uint32_t _pad0;
        uint32_t _pad1;
    };
    static_assert(sizeof(VolumetricFogPushConstants) == 80);

    struct alignas(16) VolumetricLightInjectPushConstants {
        float    scatteringIntensity;
        float    ambientIntensity;
        float    phaseAnisotropy;
        uint32_t enableShadows;
    };
    static_assert(sizeof(VolumetricLightInjectPushConstants) == 16);

    struct alignas(16) VolumetricTemporalPushConstants {
        float    temporalWeight;
        float    clampStrength;
        uint32_t resetHistory;
        uint32_t _pad;
    };
    static_assert(sizeof(VolumetricTemporalPushConstants) == 16);

    // The vkCmdPushDataEXT per-pass blob leading descriptor_heap_layout.slang's
    // DescriptorHeapPushData: the frame's matrices and GI/AO knobs, pushed once per
    // lighting/reflection draw. The generated struct, not a copy of it: its size is
    // the blob's payload region, so the shader's block may be shorter -- the push
    // check compares members.
    using ScenePassPushConstants = GeneratedGpu::ScenePassPushConstants;
    using PPPushConstants = ScenePassPushConstants;

    struct DecalPushConstants {
        JPH::Mat44 worldMatrix; // decal.slang's name for the same block
        JPH::Mat44 clipToLocal; // invWorld * unjittered invViewProj (premultiplied per frame)
        uint32_t   albedoIndex;
        uint32_t   normalIndex;
        float      roughness;
        float      metallic;
    };

    struct alignas(8) SkinningConstants {
        VkDeviceAddress inPosAddr;
        VkDeviceAddress inAttrAddr;
        VkDeviceAddress inSkinAddr;
        VkDeviceAddress outPosAddr;
        VkDeviceAddress outAttrAddr;
        VkDeviceAddress jointsAddr;
        VkDeviceAddress morphDeltasAddr;
        uint32_t        vertexCount;
        uint32_t        jointOffset;
        uint32_t        morphOffset;
        uint32_t        activeMorphCount;
        float           morphWeights[4];
    };

    // The bake type is BAKE_TYPE, a specialization constant the pipeline bakes
    // in (see RenderProcedural.cpp), not a push word: procedural_bake.slang
    // declares five members and this struct has to be exactly those five.
    struct BakePush {
        uint32_t width;
        uint32_t height;
        float    scale;
        float    randomness;
        float    distortion;
    };

    struct KawasePushConstants {
        int   mode;
        float rcpWidth;
        float rcpHeight;
        // Bright pass only: how much of the emissive channel joins the blur.
        // The down/up dispatches ignore it (it was the padding word).
        float glowIntensity;
    };

    struct RtrHalfPushConstants {
        uint32_t halfRes[2]; // half-res dispatch extent (target size)
        uint32_t _pad[2];
    };

    // ao_gtao.slang: the half-resolution GTAO horizon search. Field order
    // mirrors the Slang struct; invViewProj/viewProj land at their alignas(16)
    // offsets, so the blob stays inside DescriptorHeapPushData::passData.
    struct GtaoPushConstants {
        uint32_t halfRes[2];   // AO target extent (dispatch domain)
        float    rcpFullRes[2]; // 1 / full resolution, for the center-pixel UV
        float    time;         // noise phase (FrameUniforms.camPos.w)
        float    aoRadius;
        float    aoBias;
        float    aoPower;
        uint32_t giSamples;
        JPH::Mat44 invViewProj; // jittered, matches lighting's reconstruction
        JPH::Mat44 viewProj;    // focal length read as viewProj[1][1]
    };

    struct HdrAtrousPushConstants {
        uint32_t stepSize;   // tap spacing in pixels (1, 2, 4)
        float    phiDepth;   // depth edge-stop strength (relative to linear depth)
        float    phiNormal;  // normal edge-stop exponent
        uint32_t _pad;
    };

    struct BlitPushConstants {
        float vignetteIntensity;
        float vignettePower;
        int   fullBright;
        float exposure;
        float bloomStrength;
        float contrast;
        float saturation;
        int   tonemapper;
        float colorFilter[3];
        float _padding;
    };
    static_assert(sizeof(BlitPushConstants) == 48, "BlitPushConstants must exactly mirror blit.slang");
    static_assert(
        offsetof(BlitPushConstants, colorFilter) == 32 && offsetof(BlitPushConstants, _padding) == 44,
        "BlitPushConstants field offsets must exactly mirror blit.slang"
    );

    // SMAA.slang pushes one float4 for every stage of the chain: x = 1/width,
    // y = 1/height, z = width, w = height. One member rather than four, because
    // that is the shape the shader reads (`SMAA_RT_METRICS`).
    struct SmaaPushConstants {
        float rtMetrics[4];
    };

    // The size policy the RHI's pass concepts used to carry (they saw the scene's
    // numbers; now they see blobs). Every payload declared here that a pass pushes at
    // offset 0 must fit the push blob's prefix in front of the frame addresses --
    // GpuAbi::kScenePassPayloadBytes, the size of the largest of them, the scene-pass
    // struct itself. A payload out there, beyond this inventory, asserts the same
    // concept at its own definition; a new heap pass adds its struct to this fold.
    static_assert(
        (GpuAbi::ScenePassPayload<ComputePushConstants> && GpuAbi::ScenePassPayload<ParticleRenderPushConstants> && GpuAbi::ScenePassPayload<MeshParticleComputePush>
         && GpuAbi::ScenePassPayload<MeshParticleRenderPush> && GpuAbi::ScenePassPayload<ObjectConstants> && GpuAbi::ScenePassPayload<UIObjectConstants>
         && GpuAbi::ScenePassPayload<VolumetricFogPushConstants> && GpuAbi::ScenePassPayload<VolumetricLightInjectPushConstants>
         && GpuAbi::ScenePassPayload<VolumetricTemporalPushConstants> && GpuAbi::ScenePassPayload<ScenePassPushConstants> && GpuAbi::ScenePassPayload<DecalPushConstants>
         && GpuAbi::ScenePassPayload<SkinningConstants> && GpuAbi::ScenePassPayload<BakePush> && GpuAbi::ScenePassPayload<KawasePushConstants>
         && GpuAbi::ScenePassPayload<RtrHalfPushConstants> && GpuAbi::ScenePassPayload<GtaoPushConstants> && GpuAbi::ScenePassPayload<HdrAtrousPushConstants>
         && GpuAbi::ScenePassPayload<BlitPushConstants> && GpuAbi::ScenePassPayload<SmaaPushConstants>),
        "a pass payload no longer fits the push blob's prefix in front of the frame addresses"
    );

    struct PipelineRegistration {
        const char*              name;
        std::function<void()>    build;
        std::vector<const char*> watchPaths;
    };

    void RegisterPipeline(const PipelineRegistration& reg) noexcept;
    void ProvokeDeviceLostInternal() const;

    [[nodiscard]] std::expected<void, ErrorCode> BuildSkinningPipeline();
    void                                     DispatchSkinningPasses(VkCommandBuffer cmd);

    [[nodiscard]] std::expected<void, ErrorCode> BuildProceduralBakePipeline();
    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
        -> std::expected<uint32_t, ErrorCode>;

    void BuildTLAS(VkCommandBuffer cmd) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> InitShadowResources();
    [[nodiscard]] std::expected<void, ErrorCode> InitCullingResources();
    // Both stages arrive as descriptors the caller built from a generated module
    // (<ShaderBindings.hpp>), so the entry points are the modules' own and the
    // vertex/fragment pair cannot be mixed across the scene variants.
    [[nodiscard]] std::expected<void, ErrorCode> CompileShadowPipeline(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag);
    [[nodiscard]] std::expected<void, ErrorCode> CompilePunctualShadowPipeline(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag);
    [[nodiscard]] std::expected<void, ErrorCode> BuildDecalPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildParticlePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BuildMeshParticlePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> InitBindless();
    [[nodiscard]] std::expected<void, ErrorCode> BuildTAAPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildFXAAPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildMLAAPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildSMAAPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildLightingPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildReflectionPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BuildBlitPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildBloomPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BuildHangGpuPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> InitPostProcessing();
    [[nodiscard]] std::expected<void, ErrorCode> InitCSGPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> SetupUI();
    [[nodiscard]] std::expected<void, ErrorCode> BuildHiZPipeline();

    // Texture uploads go straight to textureManager.Upload2D / .UploadCube;
    // there is no Impl-level pass-through to route them through.

    [[nodiscard]] auto CreateGPUBuffer(size_t size, const void* data, Vk::BufferUsage functionalUsage) const
        -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, ErrorCode>;

    void BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh) const;

    [[nodiscard]] auto InitializeSystemTextures() noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto InitializeVolumetricNoiseTexture() noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto InitializeBlueNoiseTexture() -> std::expected<void, ErrorCode>;

    void RecordComputeFrame(Vk::CommandBuffer<Vk::QueueType::Compute> compCmd);
    void RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& settings);

    // Compiles a PipelineDesc into a Material: the vertex pipeline always,
    // plus the task+mesh+fragment twin when mesh blobs are provided.
    // Implemented in RenderResources.cpp.
    [[nodiscard]] auto CreatePipelineMaterial(const PipelineDesc& desc) -> std::expected<Material, ErrorCode>;

    void BeginShaderObservation();
    void HandleShaderFileEvent(const FS::FileWatchEvent& event);
    void RegisterShaderReload(std::string_view name, const std::vector<const char*>& paths, std::function<void()> callback);
    void RegisterShaderReload(std::string_view name, std::initializer_list<const char*> paths, std::function<void()> callback);

    template <VkFormat F>
    [[nodiscard]] auto CreateDefaultTarget(VkExtent2D ext, Vk::ImageUsage extraFlags = Vk::ImageUsage::None) -> std::expected<Vk::RenderTarget<F>, ErrorCode> {
        return Vk::RenderTarget<F>::Create(allocator, ctx, ext, {.usage = Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | extraFlags});
    }

    [[nodiscard]] std::expected<void, ErrorCode> RecreateTargets(VkExtent2D ext);

    // --- Graphics settings application
    // Delta-detected application of a new GraphicsSettings state: reacts to
    // resolution changes (cascade shadow target resize), then swaps in the
    // canonical state. Renderer-internal; the public entry point is
    // RenderContext::ApplySettings.
    void ApplySettings(GraphicsSettings&& incoming) noexcept;

    // Rebuilds the cascade shadow map targets at a new resolution. Returns
    // failure (leaving the current targets intact) when waiting for the
    // device or the reallocation fails.
    [[nodiscard]] std::expected<void, ErrorCode> ResizeShadowTargets(uint32_t resolution) noexcept;

    void                                     RecreatePunctualShadowViews() noexcept;
    [[nodiscard]] std::expected<void, ErrorCode> InitSkeletalAnimationResources();
    [[nodiscard]] std::expected<void, ErrorCode> InitLightingLUTs();

    [[nodiscard]] std::expected<Vk::ShaderStages, ErrorCode> LoadAndCreateShaders(VertexStageSource vs, FragmentStageSource ps) const noexcept;
    [[nodiscard]] std::expected<Vk::Pipeline, ErrorCode>
        LoadAndCreateComputeShader(ComputeStageSource cs, VkPipelineLayout layout, Vk::DynamicComputePass& pass) const noexcept;


    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ctx.BufferAddress(buffer);
    }
};

template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
auto RenderContext::Impl::BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
    -> std::expected<uint32_t, ErrorCode> {
    static_assert(Vk::GpuTriviallyCopyable<PushT>);
    return Vk::ImageBuilder {}
        .Texture2D(width, height, format, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled, 1)
        .Build(allocator.Get())
        .and_then([&](Vk::Image image) -> std::expected<uint32_t, ErrorCode> {
            auto viewRes = Vk::CreateView(ctx.Device(), image.Handle(), format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            if (!viewRes) {
                return std::unexpected(viewRes.error());
            }
            Vk::ImageView               view      = std::move(*viewRes);
            const VkImageViewCreateInfo writeInfo = Vk::MakeViewCreateInfo2D(image.Handle(), format, 1, VK_IMAGE_ASPECT_COLOR_BIT);
            // A bake is out-of-frame: BeginImmediate rewinds the bake partition,
            // and the write hands back the block this dispatch uses.
            heapManager.BeginImmediate();
            const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Declared>(
                ctx, bakeHeapBindings, Vk::Slot<"outTexture">(Vk::ImageWrite {.view = view.Get(), .viewInfo = &writeInfo})
            );

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                heapManager.BindHeaps(cmd);
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, image.Handle());
                pass.DispatchHeapIndexedThreads<Modules...>(ctx, cmd, block, width, height, 1, push);
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
            });
            return textureManager.Adopt(std::move(image), std::move(view), format);
        });
}

struct FrameRecorder {
    Vk::CommandBuffer<Vk::QueueType::Graphics> cmd;
    mutable Vk::CommandEncoder                 encoder;
    RenderContext::Impl&                       ctx;
    uint32_t                                   frameIndex;
    // VK_EXT_descriptor_heap: true when recording into a SECONDARY command
    // buffer that inherits the primary's heap bindings (ParallelCommandRecorder
    // with SetHeapState). Such secondaries must not bind their own heaps —
    // doing so would invalidate the primary's heap state after execution —
    // and the recorder already re-pushed the per-frame device-address block.
    bool heapsInherited;

    FrameRecorder(Vk::CommandBuffer<Vk::QueueType::Graphics> c, RenderContext::Impl& impl, bool inherited = false) noexcept:
        cmd(c), encoder(c.handle, &impl.ctx), ctx(impl), frameIndex(impl.presenter.frameIndex), heapsInherited(inherited) {
    }

    FrameRecorder(VkCommandBuffer c, RenderContext::Impl& impl, bool inherited = false) noexcept:
        cmd({c}), encoder(c, &impl.ctx), ctx(impl), frameIndex(impl.presenter.frameIndex), heapsInherited(inherited) {
    }

    // Binds the heaps + pushes the per-frame address block, unless the
    // surrounding secondary already inherits the heaps (and received the
    // push block from the recorder).
    void EnsureHeapState(VkCommandBuffer c) const noexcept {
        if (!heapsInherited) {
            ctx.BindHeapsAndPushFrame(c);
        }
    }

    void WriteCheckpoint(std::string_view name) const noexcept {
        ctx.WriteCheckpoint(cmd, name);
    }
};

struct GroupRange {
    const NativeMaterial* material;
    uint32_t              start;
    uint32_t              count;
};

template <typename T, typename... Args>
concept IsRenderPass = requires(T pass, Args&&... args) {
    { pass.Execute(std::forward<Args>(args)...) };
};

template <typename Pass, typename... Args>
    requires IsRenderPass<Pass, Args...>
void RunPass(const Pass& pass, Args&&... args) {
    pass.Execute(std::forward<Args>(args)...);
}

namespace Passes {

struct ShadowPass {
    static constexpr uint32_t kCubemapFaceMask  = 0x3F;
    // One layered render pass fans every draw out to all four cascade layers
    // (ViewIndex picks the light-space matrix and the implicit destination
    // layer), replacing four sequential render-target switches.
    static constexpr uint32_t kCascadeViewMask  = 0x0F;
    static constexpr float    kShadowClearDepth = 1.0f;
    void                      Execute(const FrameRecorder& recorder) const noexcept;
};

struct MainPass1 {
    void Execute(
        const FrameRecorder&                                                                                       recorder,
        SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
    ) const noexcept;
};
struct MainPass2 {
    void Execute(
        const FrameRecorder&                                                                                       recorder,
        SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
    ) const noexcept;
};

struct DeferredLightingPass {
    [[nodiscard]] auto Execute(
        const FrameRecorder&                                                                               recorder,
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in
    ) const noexcept -> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
};

struct TranslucentPrePass {
    void Execute(
        const FrameRecorder&                                             recorder,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         norm_att,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth_att
    ) const noexcept;
};

struct ForwardPass {
    void Execute(
        const FrameRecorder&                                             recorder,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>         litColor,
        Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> depth
    ) const noexcept;
};

struct BloomPass {
    [[nodiscard]] auto Execute(const FrameRecorder& recorder, Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor) const noexcept
        -> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
};

struct AAPass {
    using SceneRO      = SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
    using ColorImageRO = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

    [[nodiscard]] auto Execute(const FrameRecorder& recorder, SceneRO in) const noexcept -> SceneRO;

  private:
    [[nodiscard]] auto ExecuteTAA(VkCommandBuffer cmd, const FrameRecorder& recorder, const SceneRO& in, ColorImageRO color_ro) const noexcept -> ColorImageRO;
    [[nodiscard]] auto ExecuteFXAA(VkCommandBuffer cmd, const FrameRecorder& recorder, const SceneRO& in, ColorImageRO color_ro) const noexcept -> ColorImageRO;
    [[nodiscard]] auto ExecuteSMAA(VkCommandBuffer cmd, const FrameRecorder& recorder, const SceneRO& in, ColorImageRO color_ro) const noexcept -> ColorImageRO;
};

struct BlitPass {
    // `blockBase` is the descriptor block the caller wrote for this draw
    // (HeapManager::WriteHeapParameters): the blit's descriptors are per-frame
    // transient, so it cannot be resolved here. The 2D UI is composed by
    // `RenderContext::RenderUI` on the same attachment, never inside the blit.
    void Execute(
        const FrameRecorder&                                     recorder,
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> swapchainTarget,
        Vk::HeapBlockBase                                        blockBase,
        int                                                      fullBright
    ) const noexcept;
};

struct ViewmodelPass {
    void Execute(
        const FrameRecorder&                                                                                       recorder,
        SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL> in
    ) const noexcept;
};

} // namespace Passes

inline std::vector<uint32_t> LoadShaderSpv(const std::string& path) noexcept {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    auto                  fileSize = file.tellg();
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();
    return buffer;
}

template <ShaderStage Stage>
inline bool LoadShaderData(const ShaderStageSource<Stage>& src, const void*& outData, size_t& outSize, std::vector<uint32_t>& diskBuffer) {
    outData = src.fallback.data();
    outSize = src.fallback.size_bytes();
    if constexpr (isDev) {
        diskBuffer = LoadShaderSpv(src.path);
        if (!diskBuffer.empty()) {
            outData = diskBuffer.data();
            outSize = diskBuffer.size() * 4;
            return true;
        }
    }
    return false;
}

template <typename T = Vk::Buffer, typename... Args>
auto CreateDoubleBuffered(Vk::Allocator& alloc, Args&&... args) -> std::expected<DoubleBuffered<T>, ErrorCode> {
    return T::Create(alloc.Get(), std::forward<Args>(args)...).and_then([&](auto&& first) -> auto {
        return T::Create(alloc.Get(), std::forward<Args>(args)...).transform([&](auto&& second) -> auto {
            return DoubleBuffered<T> {std::forward<decltype(first)>(first), std::forward<decltype(second)>(second)};
        });
    });
}

} // namespace ZHLN

template <>
struct ZHLN::Vk::FormatOf<float[3]> {
    static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <>
struct ZHLN::Vk::FormatOf<::ZHLN::Packed1010102> {
    static constexpr auto value = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
};
template <>
struct ZHLN::Vk::FormatOf<::ZHLN::PackedHalf2> {
    static constexpr auto value = VK_FORMAT_R16G16_SFLOAT;
};
template <>
struct ZHLN::Vk::FormatOf<::ZHLN::PackedRGBA8> {
    static constexpr auto value = VK_FORMAT_R8G8B8A8_UNORM;
};

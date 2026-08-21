// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInternal.hpp
#pragma once
#include "../FileWatcher.hpp"
#include "Rendering.hpp"
#include "TextureManager.hpp" // Private header
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <cstddef>
#include <fstream>
#include <memory>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

struct IBLPayload {
    Image                    brdfLutImage;
    ImageView                brdfLutView;
    Image                    prefilteredImage;
    ImageView                prefilteredView;
    std::array<JPH::Vec4, 9> shCoeffs {};
};

} // namespace ZHLN::Vk

namespace ZHLN {

// ============================================================================
// Environment-Toggleable Render Diagnostics (Impl in RenderFrame.cpp)
// ============================================================================
// These exist to triage run-to-run nondeterminism in the scene pass without
// requiring RenderDoc or GPU-AV. They are read once at startup:
//   ZHLN_NO_GPU_CULLING=1  Force the CPU culling policy in MainPass1/2.
//   ZHLN_DEBUG_INDIRECT=1  Periodically log the queued draws, the retired
//                          GPU indirect commands and instance buffer data.
namespace Diag {
[[nodiscard]] bool DisableGpuCulling() noexcept;
[[nodiscard]] bool IndirectTelemetryEnabled() noexcept;
} // namespace Diag

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using EngineAS = std::conditional_t<
    isMac,
    Vk::BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S, 1, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT>,
    Vk::AccelerationStructureSlot<B, S>>;

// ============================================================================
// GenerationalPool Template
// ============================================================================

template <typename T, size_t MaxObjects, typename HandleType = uint64_t>
class GenerationalPool {
  public:
    enum class Error : uint8_t {
        Success = 0,
        InvalidHandle,    // The handle was 0/Null
        StaleHandle,      // Generational mismatch (the resource was already destroyed)
        OutOfBoundsIndex, // Index exceeds pool capacity
        NullResource      // Internal error: slot points to null pointer
    };

    GenerationalPool() {
        _freeIndices.reserve(MaxObjects);
        for (size_t i = 0; i < MaxObjects; ++i) {
            _freeIndices.push_back((uint32_t) (MaxObjects - 1 - i));
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

enum RenderAttachmentSlot : uint8_t {
    ATTACHMENT_SLOT_SCENE_COLOR      = 0,
    ATTACHMENT_SLOT_VELOCITY         = 1,
    ATTACHMENT_SLOT_ACCUM_0          = 2,
    ATTACHMENT_SLOT_ACCUM_1          = 3,
    ATTACHMENT_SLOT_NORMAL_ROUGHNESS = 4,
    ATTACHMENT_COUNT                 = 5
};

enum GBufferAttachmentSlot : uint8_t { GBUFFER_SLOT_SCENE_COLOR = 0, GBUFFER_SLOT_VELOCITY = 1, GBUFFER_SLOT_NORMAL_ROUGHNESS = 2, GBUFFER_COLOR_COUNT = 3 };

static constexpr uint32_t kGpuCullingSentinel        = 0xFFFFFFFF;
static constexpr Color4   kClearColorNormalRoughness = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};

static constexpr Color4 kClearColorBlack = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};

static constexpr uint32_t kMainPassColorAttachmentCount = 2;
static constexpr uint32_t kParallelChunkSize            = 256;

static constexpr Color4 kClearColorScene    = {.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f}; // G-Buffer background theme
static constexpr Color4 kClearColorVelocity = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr float  kClearDepthValue    = 1.0f;

// --- Layouts and Types ---
static constexpr VkShaderStageFlags kCommonStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// ----------------------------------------------------------------------------
// Per-pass descriptor layouts. Layout authority lives in the compiled shaders:
// every pass layout is reflected (SPIRV-Reflect) from its Slang-produced SPIR-V
// instead of being declared as a static C++ `DescriptorLayout<...>` type list.
// The aliases below keep the historical pass names while pointing at the single
// reflection-driven implementation.
// ----------------------------------------------------------------------------
using GlobalSceneLayout           = Vk::SlangReflectedLayout;
using TAALayout                   = Vk::SlangReflectedLayout;
using FXAALayout                  = Vk::SlangReflectedLayout;
using MLAALayout                  = Vk::SlangReflectedLayout;
using SMAAEdgeLayout              = Vk::SlangReflectedLayout;
using SMAAWeightLayout            = Vk::SlangReflectedLayout;
using SMAABlendLayout             = Vk::SlangReflectedLayout;
using AmbientLayout               = Vk::SlangReflectedLayout;
using LightingLayout              = Vk::SlangReflectedLayout;
using ReflectionLayout            = Vk::SlangReflectedLayout;
using BlitLayout                  = Vk::SlangReflectedLayout;
using BloomThresholdLayout        = Vk::SlangReflectedLayout;
using KawaseLayout                = Vk::SlangReflectedLayout;
using VolumetricClearLayout       = Vk::SlangReflectedLayout;
using VolumetricFogInjectLayout   = Vk::SlangReflectedLayout;
using VolumetricLightInjectLayout = Vk::SlangReflectedLayout;
using VolumetricIntegrationLayout = Vk::SlangReflectedLayout;
using VolumetricTemporalLayout    = Vk::SlangReflectedLayout;
using CullingLayout               = Vk::SlangReflectedLayout;
using HiZGenerateLayout           = Vk::SlangReflectedLayout;
using ClusterCullingLayout        = Vk::SlangReflectedLayout;
using BakeLayout                  = Vk::SlangReflectedLayout;
using DecalLayout                 = Vk::SlangReflectedLayout;


using ActiveGBuffer = Vk::GBufferLayout<
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>, // Index 0: sceneColor
    Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>,           // Index 1: velocityBuffer
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>           // Index 2: normalRoughnessBuffer
    >;


enum class Stage : uint8_t {
    ShadowPass,
    MainPass1,
    MainPass2,
    HiZPass,
    DecalPass,
    ViewmodelPass,
    TransPrePass,
    TransReflection,
    AAPass,
    ForwardPass,
    BloomThreshPass,
    BloomBlurHPass,
    BloomBlurVPass,
    BlitPass,
    PostProcessPass,
    VolumetricClearPass,
    VolumetricFogInjectPass,
    VolumetricLightInjectPass,
    VolumetricIntegratePass,
    VolumetricTemporalPass,
    ClusterCullingPass,
    ParticleUpdatePass,
    MeshParticleUpdatePass,
};

using FrameProfiler = Profiler::GpuProfiler<Stage>;

struct NativeMesh {
    VkDevice                     device = VK_NULL_HANDLE;
    const Vk::RayTracingContext* rtCtx  = nullptr;
    Vk::Buffer                   buffer;
    uint32_t                     vertexCount = 0;
    VkDeviceAddress              vboAddress  = 0;
    VkAccelerationStructureKHR   blas        = VK_NULL_HANDLE;
    VkDeviceAddress              blasAddress = 0;
    Vk::Buffer                   blasBuffer;

    NativeMesh() = default;
    NativeMesh(
        Vk::Buffer&&               buf,
        uint32_t                   count,
        VkDeviceAddress            vboAddr,
        VkAccelerationStructureKHR b    = VK_NULL_HANDLE,
        VkDeviceAddress            addr = 0,
        Vk::Buffer&&               bBuf = {}
    ): buffer(std::move(buf)), vertexCount(count), vboAddress(vboAddr), blas(b), blasAddress(addr), blasBuffer(std::move(bBuf)) {
    }

    ~NativeMesh() {
        if (blas != VK_NULL_HANDLE && rtCtx != nullptr) {
            rtCtx->DestroyAccelerationStructure(blas);
        }
    }
};

enum class ShaderStage : std::uint8_t { Vertex, Fragment, Compute };

template <ShaderStage Stage>
struct ShaderStageSource {
    static constexpr ShaderStage  stage = Stage;
    const char*                   path;
    std::span<const std::uint8_t> fallback;
    // nullptr → the entry-point name is reflected out of the SPIR-V module
    // (ZHLN_Internal_FindSpirvEntryPoint), so VSMain/PSMain/CSMain/Smaa* all
    // resolve automatically without per-call-site bookkeeping.
    const char* entryPoint = nullptr;
};

using VertexStageSource   = ShaderStageSource<ShaderStage::Vertex>;
using FragmentStageSource = ShaderStageSource<ShaderStage::Fragment>;
using ComputeStageSource  = ShaderStageSource<ShaderStage::Compute>;

struct NativeMaterial {
    Vk::Pipeline       pipeline;
    Vk::PipelineLayout layout;
};

static constexpr uint32_t kGpuCullingMaxInstances        = 8192;
static constexpr uint32_t kGpuCullingMaxBatches          = 256;
static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

struct DrawCommand {
    InstanceData         instanceData;
    NativeMaterial*      material;
    NativeMaterial*      prePassMaterial;
    NativeMesh*          posMesh;
    NativeMesh*          attrMesh;
    NativeMesh*          skinMesh;
    BufferHandle         skinnedVertexBuffer;
    uint32_t             jointOffset;
    uint32_t             morphOffset;
    uint32_t             activeMorphCount;
    std::array<float, 4> morphWeights;
    DrawFlags            flags;
};

static_assert(std::is_trivially_copyable_v<DrawCommand> && std::is_trivially_constructible_v<DrawCommand>);

struct CSGDrawCommand {
    DrawCommand eyeDraw;
    uint32_t    eyeInstanceIdx;

    struct Cutter {
        DrawCommand  draw;
        uint32_t     instanceIdx;
        CSGOperation operation;
    };
    ZHLN::Array<Cutter> cutters;
};

struct ParticleEmitterCommand {
    BufferHandle          gpuBuffer;
    uint32_t              maxParticles;
    ParticleEmitterParams params;
};

static_assert(std::is_trivially_copyable_v<ParticleEmitterCommand> && std::is_standard_layout_v<ParticleEmitterCommand>);

struct DecalDrawCommand {
    JPH::Mat44 transform;
    JPH::Mat44 invTransform;
    uint32_t   albedoIndex;
    uint32_t   normalIndex;
    float      roughness;
    float      metallic;
};

struct LineSegment {
    JPH::Vec3 start      = JPH::Vec3::sZero();
    JPH::Vec3 end        = JPH::Vec3::sZero();
    JPH::Vec4 colorStart = {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Vec4 colorEnd   = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct MeshParticleEmitterCommand {
    BufferHandle              gpuBuffer;
    uint32_t                  maxParticles;
    MeshParticleEmitterParams params;
    AssetID                   meshAsset;
    MaterialID                materialAsset;
};

struct WorkerCmdContext {
    std::array<Vk::CommandPool<Vk::QueueType::Graphics>, 2> pools;
    std::array<ZHLN::Atomic<uint32_t>, 2>                   cmdCount {};
};

template <VkImageLayout ColorL, VkImageLayout DepthL>
struct SceneResources {
    Vk::TypedImage<ColorL> sceneColor;
    Vk::TypedImage<ColorL> velocity;
    Vk::TypedImage<ColorL> normRough;
    Vk::TypedImage<DepthL> depth;
};

namespace Resource {
struct ShaderPair;
}

// ============================================================================
// Frame Graph Resource Tags
// ============================================================================
using Res_SceneColor    = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity      = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough     = Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Depth         = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_ShadowMap     = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas   = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Ambient       = Vk::GraphImage<"Ambient", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Lighting      = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HdrSceneColor = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomThresh   = Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown1    = Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown2    = Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown3    = Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomUp2      = Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomUp1      = Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomFinal    = Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaEdge      = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight    = Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain     = Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;
using Res_VoxelMedia    = Vk::GraphImage<"VoxelMedia", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_VoxelLight    = Vk::GraphImage<"VoxelLight", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_VoxelInt      = Vk::GraphImage<"VoxelInt", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_VoxelHist     = Vk::GraphImage<"VoxelHist", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_VoxelResolved = Vk::GraphImage<"VoxelResolved", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
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

struct RenderQueues {
    ZHLN::Array<DrawCommand>                drawQueue;
    ZHLN::Array<CSGDrawCommand>             csgDrawQueue;
    ZHLN::Array<ParticleEmitterCommand>     particleEmittersQueue;
    ZHLN::Array<MeshParticleEmitterCommand> meshParticleQueue;
    ZHLN::Array<DecalDrawCommand>           decalQueue;
    ZHLN::Array<LineSegment>                lineQueue;
    ZHLN::Array<UIBatch>                    uiBatches;

    void Clear() noexcept {
        ZHLN::Reflect::ForEachField(*this, [](auto& queue) { queue.clear(); });
    }
};

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
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     ambientTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     lightingTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     hdrSceneColor;
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
            Res_Ambient       ambientTarget;
            Res_Lighting      lightingTarget;
            Res_HdrSceneColor hdrSceneColor;
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
    static constexpr uint32_t kMaxUiVertices                 = 100'000;
    static constexpr uint32_t kGpuParticleCount              = 65'536;
    static constexpr uint32_t kGpuCullingMaxInstances        = 8'192;
    static constexpr uint32_t kGpuCullingMaxBatches          = 256;
    static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

    Window&                                      window;
    String64                                     appName;
    Vk::Context                                  ctx;
    Vk::Allocator                                allocator;
    Vk::Surface                                  surface;
    Vk::PresentationContext                      presentation;
    Vk::FrameSync<2>                             sync;
    Vk::CommandPools<2, Vk::QueueType::Graphics> pools;
    Vk::CommandPools<2, Vk::QueueType::Compute>  computePools;
    Vk::StagingRingBuffer                        stagingRingBuffer;
    mutable Vk::StagingRingBuffer                transferRingBuffer;

    mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Compute, 8>  computeCmdRing;

    VkCommandBuffer                           current_cmd = VK_NULL_HANDLE;
    Vk::CommandBuffer<Vk::QueueType::Compute> current_compute_cmd;

    std::unique_ptr<Vk::StagingContext>    stagingContext;
    Vk::DeletionQueue                      deletionQueue;
    std::optional<Vk::ScopedDeletionQueue> activeQueueGuard;

    ZHLN::Array<WorkerCmdContext>                  workerCmds;
    DoubleBuffered<Vk::ParallelCommandRecorder<2>> parallelRecorder;

    struct PendingAcquires {
        ZHLN::Mutex                         mutex {};
        ZHLN::Array<VkBufferMemoryBarrier2> buffers;

        void Drain(VkCommandBuffer cmd) noexcept {
            ZHLN::Lock(mutex, [&] {
                if (!buffers.empty()) {
                    Vk::BufferBarrier(cmd, buffers);
                    buffers.clear();
                }
            });
        }
    };
    mutable PendingAcquires pendingAcquires;

    GraphResources graphResources;

    // ============================================================================
    // Bounded Substruct for Double-Buffered Resources (Reflection-Safe for Clangd)
    // ============================================================================
    struct PerFrameResources {
        DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;
        ZHLN::DoubleBuffered<VkDescriptorSet>                           bindlessSets;
        ZHLN::DoubleBuffered<Vk::Buffer>                                lineVbos;
        ZHLN::DoubleBuffered<VkDeviceAddress>                           lineVboAddresses;
        ZHLN::DoubleBuffered<Vk::Buffer>                                uiVbos;
        ZHLN::DoubleBuffered<VkDeviceAddress>                           uiVboAddresses;
        ZHLN::DoubleBuffered<Vk::Buffer>                                clusterGridBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                lightIndexListBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                globalCounterBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                frameUniformBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                lightStorageBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                instanceDataBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                indirectCommandsBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                indirectCommandsBuffersPass2;
        ZHLN::DoubleBuffered<Vk::Buffer>                                secondPassCandidatesBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                secondPassCountBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                shadowIndirectBuffers;
        ZHLN::DoubleBuffered<Vk::Buffer>                                jointBuffers;
        ZHLN::DoubleBuffered<VkDescriptorSet>                           cullingSetsPass1;
        ZHLN::DoubleBuffered<VkDescriptorSet>                           cullingSetsPass2;
        ZHLN::DoubleBuffered<VkDescriptorSet>                           clusterCullingSets;
        ZHLN::DoubleBuffered<VkDescriptorSet>                           clusterBoundsSets;
        DoubleBuffered<VkAccelerationStructureKHR>                      tlas;
        DoubleBuffered<Vk::Buffer>                                      tlasBuffer;
        DoubleBuffered<Vk::Buffer>                                      tlasScratchBuffer;
        DoubleBuffered<Vk::Buffer>                                      tlasInstanceBuffers;
        DoubleBuffered<Vk::Buffer>                                      tlasStagingBuffers;
        ZHLN::DoubleBuffered<BufferHandle>                              debugMeshHandles;
        ZHLN::DoubleBuffered<Vk::Buffer>                                fogVolumesBuffer;

        void FlipAll() noexcept {
            ZHLN::Reflect::ForEachField(*this, [](auto& field) { FlipObject(field); });
        }
    };

    PerFrameResources frames;

    Vk::Buffer clusterBoundsBuffer;
    Vk::Buffer morphDeltasBuffer;

    Vk::SlangReflectedLayout bindlessLayout;
    Vk::DescriptorPool      bindlessPool;

    Vk::Sampler globalSampler;
    Vk::Sampler clampSampler;
    Vk::Sampler defaultSampler;
    Vk::Sampler pointSampler;

    ZHLN::Array<Vk::Image>     textureImages;
    ZHLN::Array<Vk::ImageView> textureViews;

    Vk::PostProcessPass<TAALayout>        taaPass;
    Vk::PostProcessPass<FXAALayout>       fxaaPass;
    Vk::PostProcessPass<MLAALayout>       mlaaPass;
    Vk::PostProcessPass<SMAAEdgeLayout>   smaaEdgePass;
    Vk::PostProcessPass<SMAAWeightLayout> smaaWeightPass;
    Vk::PostProcessPass<SMAABlendLayout>  smaaBlendPass;

    Vk::PostProcessPass<AmbientLayout>               ambientPass;
    Vk::PostProcessPass<LightingLayout>              lightingPass;
    Vk::PostProcessPass<ReflectionLayout>            reflectionPass;
    Vk::PostProcessPass<ReflectionLayout>            translucentReflectionPass;
    Vk::PostProcessPass<BlitLayout>                  blitPass;
    Vk::PostProcessPass<BloomThresholdLayout>        bloomThresholdPass;
    std::array<Vk::PostProcessPass<KawaseLayout>, 3> bloomDownPass;
    std::array<Vk::PostProcessPass<KawaseLayout>, 3> bloomUpPass;

    Vk::ComputePass                                            clusterBoundsPass;
    Vk::ComputePass                                            clusterCullingPass;
    Vk::ComputePass                                            cullingPass;
    Vk::ComputePass                                            skinningPass;
    Vk::ComputePass                                            proceduralBakePass;
    Vk::ComputePass                                            hangGpuPass;
    Vk::DoubleBufferedComputePass<VolumetricClearLayout>       volumetricClearPass;
    Vk::DoubleBufferedComputePass<VolumetricFogInjectLayout>   volumetricFogInjectPass;
    Vk::DoubleBufferedComputePass<VolumetricLightInjectLayout> volumetricLightInjectPass;
    Vk::DoubleBufferedComputePass<VolumetricIntegrationLayout> volumetricIntegrationPass;
    Vk::DoubleBufferedComputePass<VolumetricTemporalLayout>    volumetricTemporalPass;

    Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMapPrev;
    ZHLN::Array<Vk::ImageView>             shadowCascadeViewsPrev;

    Vk::PipelineLayout skinningPipelineLayout;
    Vk::PipelineLayout shadowPipelineLayout;
    Vk::PipelineLayout punctualShadowPipelineLayout;

    Vk::TypedPipeline<0, true> shadowPipeline;
    Vk::TypedPipeline<0, true> punctualShadowPipeline;

    // Encapsulated Texture Lifecycle Manager
    TextureManager textureManager;

    Vk::Buffer                  particleBuffer;
    Vk::ComputePass             particleUpdatePass;
    Vk::PipelineLayout          particleRenderLayout;
    Vk::TypedPipeline<1, false> particleRenderPipeline;

    Vk::ComputePass    meshParticleUpdatePass;
    Vk::PipelineLayout meshParticleRenderLayout;
    Vk::Pipeline       meshParticleRenderPipeline;
    Vk::Pipeline       meshParticleShadowPipeline;

    Vk::SlangReflectedLayout decalDescLayout;
    Vk::DescriptorPool      decalDescPool;
    VkDescriptorSet         decalSet = VK_NULL_HANDLE;

    Vk::PipelineLayout decalPipelineLayout;
    Vk::Pipeline       decalPipeline;

    Vk::PipelineLayout linePipelineLayout;
    Vk::Pipeline       linePipeline;
    uint32_t           activeLineVertexCount = 0;
    uint32_t           lineInstanceId        = 0;

    std::expected<void, Error> BuildLinePipeline();
    std::expected<void, Error> InitLineBuffers() noexcept;
    std::expected<void, Error> AllocateDynamicVertexBuffers(
        size_t maxVertices, DoubleBuffered<Vk::Buffer>& bufs, DoubleBuffered<VkDeviceAddress>& addrs, VkBufferUsageFlags extraFlags, const char* label
    ) noexcept;
    void                       FlushLineQueue();

    // --- Indirect-draw telemetry (enabled via ZHLN_DEBUG_INDIRECT=1) ---
    // The GPU-only indirect/counter buffers cannot be mapped, so the frame
    // copies their heads into a small host-visible readback buffer at the end
    // of recording; the dump two frames later (slot retired) reads that copy.
    static constexpr uint32_t kTelemetryMaxDraws      = 8;
    static constexpr size_t   kTelemetryPass1Offset   = 0;
    static constexpr size_t   kTelemetryPass2Offset   = sizeof(VkDrawIndirectCommand) * kTelemetryMaxDraws;
    static constexpr size_t   kTelemetryCountOffset   = kTelemetryPass2Offset * 2;
    static constexpr size_t   kTelemetryReadbackBytes = kTelemetryCountOffset + sizeof(uint32_t);

    std::array<Vk::Buffer, 2> indirectReadbackBuffers {};
    bool                      indirectReadbackReady = false;

    void RecordIndirectTelemetry(VkCommandBuffer cmd) noexcept;
    void DumpIndirectTelemetry(uint32_t frameNo) noexcept;

    Vk::SlangReflectedLayout      cullingLayout;
    Vk::DescriptorPool           cullingPool;
    Vk::ComputePass              hizGeneratePass;
    Vk::SlangReflectedLayout      hizDescLayout;
    Vk::DescriptorPool           hizPool;
    ZHLN::Array<VkDescriptorSet> hizSets;

    Vk::SlangReflectedLayout clusterCullingDescLayout;
    Vk::DescriptorPool      clusterCullingPool;

    Vk::SlangReflectedLayout clusterBoundsDescLayout;
    Vk::DescriptorPool      clusterBoundsPool;

    Vk::SlangReflectedLayout proceduralBakeDescLayout;
    Vk::DescriptorPool      proceduralBakeDescPool;
    VkDescriptorSet         proceduralBakeSet = VK_NULL_HANDLE;

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
    ZHLN::HashMap<uint64_t, BufferHandle> particleBufferMap;

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> tracked2DEmitters;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>> tracked3DEmitters;

    RenderQueues          queues;
    ZHLN::Array<GPULight> mappedLights;

    Vk::Pipeline       csgWritePipeline;
    Vk::Pipeline       csgDifferencePipeline;
    Vk::Pipeline       csgIntersectionPipeline;
    Vk::PipelineLayout csgPipelineLayout;

    Vk::DescriptorPool uiPool;
    Vk::Pipeline       uiPipeline;
    Vk::PipelineLayout uiPipelineLayout;

    std::expected<void, Error> InitUIDynamicBuffers() noexcept;

    Vk::RayTracingContext rtCtx;

    JPH::Mat44    current_view_proj    = JPH::Mat44::sIdentity();
    JPH::Mat44    unjittered_view_proj = JPH::Mat44::sIdentity();
    JPH::Mat44    shadowProjView       = JPH::Mat44::sIdentity();
    FrameUniforms currentUniforms {};
    float         currentDt = 0.0166f;

    GISettings    giSettings {};
    AAState       aaState {};
    FrameProfiler gpuProfiler;

    struct WatchableShader {
        std::string           path;
        FileWatcher           watcher;
        std::function<void()> reloadCallback;
    };

    ZHLN::Array<WatchableShader> shaderWatchers;

    uint32_t frame_index         = 0;
    uint32_t current_image_index = 0;
    uint32_t nextTextureIndex    = 0;
    uint32_t nextMorphDeltaIndex = 0;
    uint32_t smaaAreaTexIdx      = 0;
    uint32_t smaaSearchTexIdx    = 0;

    float lastAspectRatio = 0.0f;
    float lastFov         = 0.0f;

    bool resized             = true;
    bool needsInitialClear   = true;
    bool depth_ready         = false;
    bool hasSkinnedThisFrame = false;

    ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;
    ZHLN::Array<SortItem>                           sortItemsScratch;
    ZHLN::Array<SortItem>                           sortTempScratch;
    ZHLN::Array<DrawCommand>                        sortDrawQueueScratch;

    Impl(Window& win): window(win) {
    }

    ~Impl() {
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

    [[nodiscard]] std::expected<void, Error> InitSubsystems(const RenderConfig& cfg, int width, int height);

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

        uint32_t cascadeIndex;
    };
    static_assert(sizeof(MeshParticleRenderPush) == 104);

    struct PPPushConstants {
        JPH::Mat44 invViewProj;
        JPH::Mat44 viewProj;
        alignas(16) std::array<float, 4> camPos;
        int   giMode;
        float aoRadius;
        float aoBias;
        float aoPower;
        float giIntensity;
        int   giSamples;
        int   enableSSR;
        int   enableRTR;
        int   _pad;
    };

    struct DecalPushConstants {
        JPH::Mat44 world;
        JPH::Mat44 invWorld;
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

    struct BakePush {
        uint32_t width;
        uint32_t height;
        float    scale;
        float    randomness;
        float    distortion;
        uint32_t bakeType;
    };

    struct KawasePushConstants {
        int   mode;
        float rcpWidth;
        float rcpHeight;
        float padding;
    };

    struct BlitPushConstants {
        float vignetteIntensity;
        float vignettePower;
        int   fullBright;
    };

    struct PipelineRegistration {
        const char*              name;
        std::function<void()>    build;
        std::vector<const char*> watchPaths;
    };

    void RegisterPipeline(const PipelineRegistration& reg) noexcept;
    void ProvokeDeviceLostInternal() const;

    [[nodiscard]] std::expected<void, Error> BuildSkinningPipeline();
    void                                     DispatchSkinningPasses();

    [[nodiscard]] std::expected<void, Error> BuildProceduralBakePipeline();
    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
        -> std::expected<uint32_t, Error>;

    void BuildTLAS(VkCommandBuffer cmd) noexcept;

    [[nodiscard]] std::expected<void, Error> InitShadowResources();
    [[nodiscard]] std::expected<void, Error> InitCullingResources();
    [[nodiscard]] std::expected<void, Error> CompileShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
    [[nodiscard]] std::expected<void, Error> CompilePunctualShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
    [[nodiscard]] std::expected<void, Error> BuildDecalPipeline();
    [[nodiscard]] std::expected<void, Error> BuildParticlePipelines();
    [[nodiscard]] std::expected<void, Error> BuildMeshParticlePipelines();
    [[nodiscard]] std::expected<void, Error> InitBindless();
    [[nodiscard]] std::expected<void, Error> BuildTAAPipeline();
    [[nodiscard]] std::expected<void, Error> BuildFXAAPipeline();
    [[nodiscard]] std::expected<void, Error> BuildMLAAPipeline();
    [[nodiscard]] std::expected<void, Error> BuildSMAAPipeline();
    [[nodiscard]] std::expected<void, Error> BuildAmbientPipeline();
    [[nodiscard]] std::expected<void, Error> BuildLightingPipeline();
    [[nodiscard]] std::expected<void, Error> BuildReflectionPipelines();
    [[nodiscard]] std::expected<void, Error> BuildBlitPipeline();
    [[nodiscard]] std::expected<void, Error> BuildBloomPipelines();
    [[nodiscard]] std::expected<void, Error> BuildHangGpuPipeline();
    [[nodiscard]] std::expected<void, Error> InitPostProcessing();
    [[nodiscard]] std::expected<void, Error> InitCSGPipelines();
    [[nodiscard]] std::expected<void, Error> SetupUI(GLFWwindow* window);
    [[nodiscard]] std::expected<void, Error> BuildHiZPipeline();

    [[nodiscard]] auto CreateTextureInternal(const void* data, uint32_t width, uint32_t height, bool isSRGB) -> std::expected<uint32_t, Error>;
    [[nodiscard]] auto CreateTextureCubeInternal(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error>;

    [[nodiscard]] std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, VkResult>
        CreateGPUBuffer(size_t size, const void* data, VkBufferUsageFlags functionalUsage) const;

    void BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh) const;

    void               SortDrawQueue();
    [[nodiscard]] auto InitializeSystemTextures() noexcept -> std::expected<void, Error>;

    void RecordComputeFrame(Vk::CommandBuffer<Vk::QueueType::Compute> compCmd);
    void RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd);

    void RegisterShaderWatcher(const char* path, std::function<void()> callback);
    void CheckShaderWatchers() noexcept;

    template <VkFormat F>
    [[nodiscard]] auto CreateDefaultTarget(VkExtent2D ext, VkImageUsageFlags extraFlags = 0) {
        return Vk::RenderTarget<F>::Create(allocator, ctx, ext, {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraFlags});
    }

    bool RecreateTargets(VkExtent2D ext);

    void                                     RecreatePunctualShadowViews() noexcept;
    [[nodiscard]] std::expected<void, Error> InitSkeletalAnimationResources();
    [[nodiscard]] std::expected<void, Error> InitLightingLUTs();

    [[nodiscard]] std::expected<Vk::ShaderStages, Error> LoadAndCreateShaders(VertexStageSource vs, FragmentStageSource ps) const noexcept;
    [[nodiscard]] std::expected<Vk::Pipeline, Error>     LoadAndCreateComputeShader(ComputeStageSource cs, VkPipelineLayout layout) const noexcept;

    void WatchPipeline(const char* vsPath, const char* psPath, std::function<void()> rebuild_fn) noexcept;
    void UploadClusterBounds(const JPH::Mat44& proj);

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ctx.BufferAddress(buffer);
    }
};

struct FrameRecorder {
    Vk::CommandBuffer<Vk::QueueType::Graphics> cmd;
    mutable Vk::CommandEncoder                 encoder;
    RenderContext::Impl&                       ctx;
    uint32_t                                   frameIndex;
    VkDescriptorSet                            bindlessSet;

    FrameRecorder(Vk::CommandBuffer<Vk::QueueType::Graphics> c, RenderContext::Impl& impl) noexcept:
        cmd(c), encoder(c.handle), ctx(impl), frameIndex(impl.frame_index), bindlessSet(impl.frames.bindlessSets[impl.frame_index]) {
    }

    FrameRecorder(VkCommandBuffer c, RenderContext::Impl& impl) noexcept:
        cmd({c}), encoder(c), ctx(impl), frameIndex(impl.frame_index), bindlessSet(impl.frames.bindlessSets[impl.frame_index]) {
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

  private:
    [[nodiscard]] constexpr uint32_t DetermineLightingVariant(const GISettings& gi, bool hasRt) const noexcept;
    [[nodiscard]] constexpr uint32_t DetermineReflectionVariant(const GISettings& gi, bool hasRt) const noexcept;
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
    void Execute(
        const FrameRecorder&                                     recorder,
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
        Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> swapchainTarget,
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
    auto                  fileSize = (size_t) file.tellg();
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
std::expected<DoubleBuffered<T>, VkResult> CreateDoubleBuffered(Vk::Allocator& alloc, Args&&... args) {
    return T::Create(alloc.Get(), std::forward<Args>(args)...).and_then([&](auto&& first) -> std::expected<DoubleBuffered<T>, VkResult> {
        return T::Create(alloc.Get(), std::forward<Args>(args)...).transform([&](auto&& second) {
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

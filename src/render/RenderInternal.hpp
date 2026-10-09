// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "DrawCommands.hpp"
#include "DrawQueueManager.hpp"
#include "FrameDestinations.hpp"
#include "GenerationalPool.hpp"
#include "GeometryManager.hpp"
#include "GpuAbi.hpp"
#include "GpuLayout.hpp"
#include "PipelineFormats.hpp"
#include "PipelineRegistry.hpp"
#include "PresentationTarget.hpp"
#include "Rendering.hpp"
#include "ShaderReloadRegistry.hpp"
#include "TargetManager.hpp"
#include "TextureManager.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "features/PostProcessFeature.hpp"
#include "features/ShadowRenderer.hpp"
#include "features/VolumetricFogSystem.hpp"
#include "graph/RenderGraph.hpp"
#include "pipeline/ComputePass.hpp"
#include "pipeline/FullscreenPass.hpp"
#include "pipelines/ReflectionPipeline.hpp"
#include "ui/UIRenderer.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZHLN {

struct TaskSystemScheduler {
    template <typename... Tasks>
    void Dispatch(Tasks&&... tasks) const {
        TaskSystem::ParallelInvoke(std::forward<Tasks>(tasks)...);
    }
};

} // namespace ZHLN

namespace ZHLN::Vk {

struct IBLPayload {
    Image     brdfLutImage;
    ImageView brdfLutView;
    Image     prefilteredImage;
    ImageView prefilteredView;
    // Original RGBA32F sky, only retained when the lighting cube was cooked
    // from a different (sunless) panorama and the skybox is visible.
    Image                    visualSkyImage;
    ImageView                visualSkyView;
    std::array<JPH::Vec4, 9> shCoeffs {};
    VkFormat                 prefilteredFormat = VK_FORMAT_R8G8B8A8_UNORM;
    uint64_t                 contentHash       = 0;
    int                      environmentMode   = 0;

    void Destroy(Allocator& allocator) noexcept {
        brdfLutView     = {};
        prefilteredView = {};
        visualSkyView   = {};
        allocator.DestroyImage(brdfLutImage);
        allocator.DestroyImage(prefilteredImage);
        allocator.DestroyImage(visualSkyImage);
    }
};

} // namespace ZHLN::Vk

namespace ZHLN {

[[nodiscard]] inline auto AsFrameError(ErrorCode code) noexcept -> ErrorCode {
    return Vk::IsDeviceLost(code) ? ErrorCode {FrameResult::DeviceLost} : code;
}

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept;

namespace Diag {
[[nodiscard]] bool DisableGpuCulling() noexcept;
[[nodiscard]] bool ForkSequentialForced() noexcept;
} // namespace Diag

static constexpr uint32_t kGpuCullingSentinel        = 0xFFFFFFFF;
static constexpr Color4   kClearColorNormalRoughness = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};

static constexpr uint32_t kParallelChunkSize = 256;

static constexpr uint32_t kSceneStaticResourceSlots = 16;
static constexpr uint32_t kSceneStaticSamplerSlots  = 16;
static_assert(kSceneStaticSamplerSlots >= 3 + kMaterialSamplerVariantCount);
static constexpr uint32_t kFrameTransientResourceSlots     = 4096;
static constexpr uint32_t kImmediateTransientResourceSlots = 64;
static constexpr uint32_t kPassStaticSamplerSlots          = 64;

static constexpr Color4 kClearColorScene      = {.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f};
static constexpr Color4 kClearColorVelocity   = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr Color4 kClearColorEmissive   = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr Color4 kClearColorClearcoat  = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr Color4 kClearColorAnisotropy = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}; // A is glTF occlusion.
static constexpr Color4 kClearColorSheen      = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr float  kClearDepthValue      = 1.0f;

static constexpr VkShaderStageFlags kCommonStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

using GlobalSceneLayout    = Vk::ReflectedLayout;
using TAALayout            = Vk::ReflectedLayout;
using FXAALayout           = Vk::ReflectedLayout;
using MLAALayout           = Vk::ReflectedLayout;
using SMAAEdgeLayout       = Vk::ReflectedLayout;
using SMAAWeightLayout     = Vk::ReflectedLayout;
using SMAABlendLayout      = Vk::ReflectedLayout;
using LightingLayout       = Vk::ReflectedLayout;
using BlitLayout           = Vk::ReflectedLayout;
using CullingLayout        = Vk::ReflectedLayout;
using HiZGenerateLayout    = Vk::ReflectedLayout;
using ClusterCullingLayout = Vk::ReflectedLayout;
using BakeLayout           = Vk::ReflectedLayout;
using DecalLayout          = Vk::ReflectedLayout;

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

static constexpr uint32_t kGpuCullingMaxInstances        = 8192;
static constexpr uint32_t kGpuCullingMaxBatches          = 256;
static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

struct WorkerCmdContext {
    std::array<Vk::CommandPool<Vk::QueueType::Graphics>, Vk::kFramesInFlight> pools;
    std::array<ZHLN::Atomic<uint32_t>, Vk::kFramesInFlight>                   cmdCount {};
};

template <VkImageLayout ColorL, VkImageLayout DepthL>
struct SceneResources {
    Vk::TypedImage<ColorL> sceneColor;
    Vk::TypedImage<ColorL> velocity;
    Vk::TypedImage<ColorL> normRough;
    Vk::TypedImage<ColorL> emissive;
    Vk::TypedImage<ColorL> clearcoat;
    Vk::TypedImage<ColorL> anisotropy;
    Vk::TypedImage<ColorL> sheen;
    Vk::TypedImage<DepthL> depth;
};

namespace Resource {
}

struct RenderContext::Impl {
    using GraphResources = TargetManager::GraphResources;

    struct RenderState {
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> initialState;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           finalColor;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           bloomFinal;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> resourcesForAA;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> aaResult;
    };

    static constexpr uint32_t SHADOW_RES          = TargetManager::kShadowResolution;
    static constexpr uint32_t NUM_CASCADES        = TargetManager::kCascades;
    static constexpr uint32_t MAX_PUNCTUAL_LIGHTS = TargetManager::kPunctualLights;

    static constexpr uint32_t kMaxLineVertices               = 500'000;
    static constexpr uint32_t kMaxDebugVertices              = 500'000;
    static constexpr uint32_t kGpuCullingMaxInstances        = 8'192;
    static constexpr uint32_t kGpuCullingMaxBatches          = 256;
    static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

    PresentationTarget&                                           presentationTarget;
    String64                                                      appName;
    Vk::Context                                                   ctx;
    Vk::PipelineCache                                             pipelineCache;
    std::string                                                   pipelineCachePath;
    Vk::Allocator                                                 allocator;
    Vk::SwapchainPresenter                                        presenter;
    PresentationMode                                              presentationMode = PresentationMode::NativeSwapchain;
    Vk::CommandPools<Vk::kFramesInFlight, Vk::QueueType::Compute> computePools;
    Vk::StagingRingBuffer                                         stagingRingBuffer;
    mutable Vk::StagingRingBuffer                                 transferRingBuffer;

    mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Compute, 8>  computeCmdRing;

    // Multi-pass subsystems. Each owns the pipelines and scratch assets its
    // passes need, so a pass is a description of recording work rather than a
    // holder of GPU state.
    ShadowRenderer      shadows;
    VolumetricFogSystem fog;
    PostProcessFeature  postProcess;

    std::unique_ptr<Vk::SubmittedStagingWork> submittedStaging;
    Vk::DeletionQueue                         deletionQueue;
    bool                                      frameOpen = false;

    static constexpr size_t                                        kParallelRecordingSlots = 2; // concurrent secondary recordings, not frames in flight
    ZHLN::Array<WorkerCmdContext>                                  workerCmds;
    PerFrame<Vk::ParallelCommandRecorder<kParallelRecordingSlots>> parallelRecorders;

    TargetManager   targets;
    GraphResources& graphResources = targets.Graph();

    uint32_t viewportX = 0;
    uint32_t viewportY = 0;
    uint32_t viewportW = 0;
    uint32_t viewportH = 0;

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

    struct PerFrameResources {
        PerFrame<Vk::Buffer>      lineVbos;
        PerFrame<VkDeviceAddress> lineVboAddresses;
        PerFrame<Vk::Buffer>      clusterGridBuffers;
        PerFrame<Vk::Buffer>      lightIndexListBuffers;
        PerFrame<Vk::Buffer>      globalCounterBuffers;
        PerFrame<Vk::Buffer>      frameUniformBuffers;
        PerFrame<Vk::Buffer>      lightStorageBuffers;
        PerFrame<Vk::Buffer>      instanceDataBuffers;
        PerFrame<Vk::Buffer>      indirectCommandsBuffers;
        PerFrame<Vk::Buffer>      indirectCommandsBuffersPass2;
        PerFrame<Vk::Buffer>      secondPassCandidatesBuffers;
        PerFrame<Vk::Buffer>      secondPassCountBuffers;
        // Joint data is addressed by both the current and previous frame slot.
        // Its physical lifetime follows the N-slot frame ring, not PingPong.
        PerFrame<Vk::Buffer>                jointBuffers;
        PerFrame<Vk::AccelerationStructure> tlas;
        PerFrame<Vk::Buffer>                tlasBuffer;
        PerFrame<Vk::Buffer>                tlasScratchBuffer;
        PerFrame<Vk::Buffer>                tlasInstanceBuffers;
        PerFrame<BufferHandle>              debugMeshHandles;
        PerFrame<Vk::Buffer>                fogVolumesBuffer;
    };

    PerFrameResources                                         frames;
    PingPong<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumulationHistory;

    Vk::Buffer clusterBoundsBuffer;
    Vk::Buffer morphDeltasBuffer;

    Vk::ReflectedLayout bindlessLayout;

    Vk::HeapManager heapManager;

    Vk::HeapMappingBundle sceneHeapMappings;
    Vk::HeapMappingBundle decalSceneHeapMappings;
    Vk::HeapMappingBundle decalHeapMappings;

    Vk::HeapPassBindings hizHeapBindings;
    Vk::HeapPassBindings cullingHeapBindings;
    Vk::HeapPassBindings clusterBoundsHeapBindings;
    Vk::HeapPassBindings clusterCullingHeapBindings;
    Vk::HeapPassBindings bakeHeapBindings;
    Vk::HeapPassBindings iblBakeHeapBindings;
    Vk::HeapPassBindings volumetricClearHeapBindings;
    Vk::HeapPassBindings volumetricFogInjectHeapBindings;
    Vk::HeapPassBindings volumetricLightInjectHeapBindings;
    Vk::HeapPassBindings volumetricIntegrationHeapBindings;
    Vk::HeapPassBindings volumetricTemporalHeapBindings;

    Vk::SamplerConfig globalSamplerConfig {};
    Vk::SamplerConfig clampSamplerConfig = Vk::SamplerConfig::LinearClampToEdge();
    Vk::SamplerConfig shadowSamplerConfig {};
    Vk::SamplerConfig defaultSamplerConfig {};
    Vk::SamplerConfig pointSamplerConfig {};
    Vk::SamplerConfig blueNoiseSamplerConfig {};

    Vk::SamplerHandle globalSamplerSlot;
    Vk::SamplerHandle materialSamplerBaseSlot; // Nine contiguous S/T wrap combinations.
    Vk::SamplerHandle clampSamplerSlot;
    Vk::SamplerHandle pointSamplerSlot;
    Vk::TextureHandle iblPrefilteredSlot;
    Vk::TextureHandle iblBrdfLutSlot;
    Vk::TextureHandle transLightingSlot;
    Vk::TextureHandle decalDepthSlot;

    VkPipelineLayout emptyPipelineLayout = VK_NULL_HANDLE;

    Vk::Sampler globalSampler;
    Vk::Sampler clampSampler;
    Vk::Sampler defaultSampler;
    Vk::Sampler pointSampler;

    Vk::Sampler blueNoiseSampler;

    Vk::FullscreenPass<TAALayout>        taaPass;
    Vk::FullscreenPass<FXAALayout>       fxaaPass;
    Vk::FullscreenPass<MLAALayout>       mlaaPass;
    Vk::FullscreenPass<SMAAEdgeLayout>   smaaEdgePass;
    Vk::FullscreenPass<SMAAWeightLayout> smaaWeightPass;
    Vk::FullscreenPass<SMAABlendLayout>  smaaBlendPass;

    Vk::FullscreenPass<LightingLayout> lightingPass;
    Vk::FullscreenPass<BlitLayout>     blitPass;

    Vk::FixedComputePass   clusterBoundsPass;
    Vk::FixedComputePass   clusterCullingPass;
    Vk::DynamicComputePass cullingPass;
    Vk::DynamicComputePass skinningPass;
    Vk::DynamicComputePass proceduralBakePass;
    Vk::DynamicComputePass hangGpuPass;
    Vk::PipelineLayout     skinningPipelineLayout;

    bool enableMeshShading = true;

    [[nodiscard]] bool MeshShadingActive() const noexcept {
        return enableMeshShading && ctx.MeshShadersSupported();
    }

    TextureManager textureManager;

    struct EmitterStorage {
        BufferHandle buffer = BufferHandle::Invalid;
        uint32_t     capacity = 0;
        uint64_t     lastSeenFrame = 0;
        bool         allocationWarningLogged = false;
    };
    using EmitterStorageMap = HashMap<uint64_t, EmitterStorage>;
    EmitterStorageMap particleEmitters;
    EmitterStorageMap meshParticleEmitters;

    Vk::DynamicComputePass      particleUpdatePass;
    VkPipelineLayout            particleRenderLayout = VK_NULL_HANDLE;
    Vk::TypedPipeline<1, false> particleRenderPipeline;

    Vk::DynamicComputePass meshParticleUpdatePass;
    VkPipelineLayout       meshParticleRenderLayout = VK_NULL_HANDLE;
    Vk::Pipeline           meshParticleRenderPipeline;
    Vk::Pipeline           meshParticleShadowPipeline;

    Vk::ReflectedLayout decalDescLayout;
    VkPipelineLayout    decalPipelineLayout = VK_NULL_HANDLE;
    Vk::Pipeline        decalPipeline;

    VkPipelineLayout linePipelineLayout = VK_NULL_HANDLE;
    Vk::Pipeline     linePipeline;
    uint32_t                  activeLineVertexCount = 0;
    uint32_t                  lineInstanceId        = 0;
    std::span<const LineSegment> pendingLines {};

    std::expected<void, ErrorCode> BuildLinePipeline();
    std::expected<void, ErrorCode> InitLineBuffers() noexcept;
    std::expected<void, ErrorCode> AllocateDynamicVertexBuffers(
        size_t                     maxVertices,
        PerFrame<Vk::Buffer>&      bufs,
        PerFrame<VkDeviceAddress>& addrs,
        Vk::BufferUsage            extraFlags = Vk::BufferUsage::None
    ) noexcept;
    void FlushLineQueue(std::span<const LineSegment> lines);

    [[nodiscard]] auto FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount>;
    void               BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept;

    std::expected<void, ErrorCode> InitSceneHeaps(const Vk::SamplerConfig& globalSamplerValue, const Vk::SamplerConfig& clampSamplerValue) noexcept;
    void                           BuildSceneHeapMappings() noexcept;
    void                           BuildDecalHeapMappings() noexcept;
    void                           WriteSceneStaticImageDescriptors() noexcept;
    void                           WritePointSamplerToHeap(const Vk::SamplerConfig& config) noexcept;
    void                           WriteTransLightingToHeap() noexcept;
    void                           InitPassSamplerDescriptors() noexcept;
    [[nodiscard]] std::expected<void, ErrorCode> InitBakeHeapBindings() noexcept;
    template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
    [[nodiscard]] auto BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
        -> std::expected<uint32_t, ErrorCode>;

    Vk::ReflectedLayout    cullingLayout;
    Vk::DynamicComputePass hizGeneratePass;
    Vk::ReflectedLayout    hizDescLayout;

    Vk::ReflectedLayout clusterCullingDescLayout;
    Vk::ReflectedLayout clusterBoundsDescLayout;

    Vk::ReflectedLayout proceduralBakeDescLayout;
    Vk::ReflectedLayout iblBakeDescLayout;

    Vk::Sampler shadowSampler;

    Vk::Image     ltcMatImage;
    Vk::ImageView ltcMatView;
    Vk::Image     ltcAmpImage;
    Vk::ImageView ltcAmpView;

    Vk::IBLPayload iblPayload;

    GeometryManager geometry;

    DrawQueueManager queues;

    // Billboards: renderer-owned storage, one slot per (texture, blend, facing, call) --
    // a host that draws the same texture twice in a frame gets two slots rather than one
    // overwritten buffer, and the serial restarts when the emitter queue empties, which
    // is the frame boundary. Slots are reused frame after frame, so a steady host
    // allocates nothing after its first frame.
    struct BillboardSlot {
        uint32_t     textureIndex = 0;
        uint32_t     blendMode    = 0;
        uint32_t     alignment    = 0;
        uint32_t     serial       = 0;
        BufferHandle buffer       = BufferHandle::Invalid;
        uint32_t     capacity     = 0;
    };
    ZHLN::Array<BillboardSlot> billboardSlots;
    ZHLN::Array<Particle>      billboardStaging;
    uint32_t                   billboardSerial = 0;

    ZHLN::Array<Light> mappedLights;

    // SetLights takes descriptions; this holds the packed structs for the frame
    // a pass is about to upload, so the conversion happens once per frame
    // instead of per consumer.
    ZHLN::Array<Light> gpuLights;
    // What SetLights was handed this frame, in the engine's own terms: the pack
    // needs the frame's view matrix (a light's positionView), and the frame is
    // handed over after the systems that submit lights have run. So the pack --
    // with the upload it feeds -- happens in SetFrameData.
    ZHLN::Array<LightDesc> submittedLights;

    uint32_t packedLightCount = 0;

    Vk::Pipeline     csgWritePipeline;
    Vk::Pipeline     csgDifferencePipeline;
    Vk::Pipeline     csgIntersectionPipeline;
    VkPipelineLayout csgPipelineLayout = VK_NULL_HANDLE;

    UIRenderer uiRenderer;

    FrameDestinations destinations;

    struct RenderTexture {
        Vk::ImageSlice       image {};
        uint32_t             bindlessIndex = 0;
        Vk::AttachmentLayout layout        = Vk::AttachmentLayout::Undefined;
        bool                 drawn         = false;
    };
    std::unordered_map<RenderTextureHandle, RenderTexture> renderTextures;

    // Process-wide IDs prevent stale handles or frame capabilities from
    // aliasing resources in a new renderer after device-loss recovery.
    static inline std::atomic<uint64_t> nextRenderTextureId {1};
    static inline std::atomic<uint64_t> nextRendererId {1};
    uint64_t                            rendererId = nextRendererId.fetch_add(1, std::memory_order::relaxed);
    uint64_t                            frameSerial = 0;
    uint64_t                            nextAcquisition = 1;
    std::atomic<uint32_t> invalidDrawWarningCount {0};
    bool warnedUnwrittenTarget = false;

    struct ForkReplayer {
        explicit ForkReplayer(RenderContext::Impl& self) noexcept: impl(&self) {
        }
        RenderContext::Impl* impl;
        [[nodiscard]] auto   ForkSecondariesActive() const noexcept -> bool {
            return impl->frameState.inForkSecondary;
        }
        void ExecuteFork(VkCommandBuffer cmd, std::span<const Vk::ForkCall> bodies) noexcept;
    };
    static_assert(Vk::ForkRecorder<ForkReplayer>);
    std::unique_ptr<ForkReplayer> forkReplayer;

    struct FrameTransientState {
        bool computeSubmitted = false;

        bool inForkSecondary = false;

        bool hasSkinned = false;

        bool resized            = true;
        bool clusterBoundsDirty = true;

        void Reset() noexcept {
            computeSubmitted = false;
            inForkSecondary  = false;
            hasSkinned       = false;
        }
    };

    FrameTransientState frameState;

    [[nodiscard]] auto ForkExecutor() noexcept -> ForkReplayer* {
        return forkReplayer.get();
    }
    [[nodiscard]] auto InheritsHeaps() const noexcept -> bool {
        return frameState.inForkSecondary;
    }

    struct DestinationVend {
        FrameDestinations::Window* entry   = nullptr;
        bool                       created = false;
    };

    [[nodiscard]] auto FindOrCreateDestination(const PresentationTarget& aux, bool primary) noexcept -> std::expected<DestinationVend, ErrorCode>;

    [[nodiscard]] auto
        ReconcileDestination(FrameDestinations::Window& dest, Vk::CommandRecorder& recorder) noexcept -> std::expected<Vk::AttachmentLayout, ErrorCode>;
    [[nodiscard]] auto TargetAttachment(const PresentationTarget& aux) const noexcept -> std::optional<FrameTarget>;
    [[nodiscard]] auto AcquireTarget(const PresentationTarget& aux) noexcept -> FrameOutcome<FrameTarget>;

    struct ResolvedTarget {
        FrameDestinations::Window& window;
        Vk::CommandRecorder&       recorder;
        Vk::ImageSlice             image;
        Vk::AttachmentLayout&      layout;
        bool&                      drawn;
    };
    [[nodiscard]] auto ResolveTarget(const FrameTarget& target) noexcept -> std::expected<ResolvedTarget, ErrorCode>;
    [[nodiscard]] auto FrameCommand() const noexcept -> VkCommandBuffer;
    void               ReleaseTarget(const PresentationTarget& aux) noexcept;
    void               DestroyDestinations() noexcept;
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<RenderTextureHandle, ErrorCode>;
    void               DestroyRenderTexture(RenderTextureHandle handle) noexcept;
    [[nodiscard]] auto EnsureEmitterStorage(EmitterStorageMap& emitters, uint64_t emitterId, uint32_t maxParticles, size_t particleStride)
        -> BufferHandle;
    void EvictInactiveEmitters() noexcept;
    void ClearEmitterBuffers() noexcept;

    [[nodiscard]] auto PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal>;

    // Non-owning view while a scene graph is executing. Never cached across
    // a frame or used as an alternate path around ResolveTarget.
    std::optional<Vk::ImageSlice> sceneTarget;

    [[nodiscard]] auto ActivePresentation() noexcept -> Vk::SwapchainPresenter& {
        if (const auto active = destinations.Active()) {
            return active->Presenter();
        }
        return presenter;
    }

    void ApplySceneView(const SceneView& view) noexcept;

    void PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept;

    JPH::Mat44 current_view_proj    = JPH::Mat44::sIdentity();
    JPH::Mat44 unjittered_view_proj = JPH::Mat44::sIdentity();
    // Kept for the pack boundary: light packing needs a world-to-view matrix, and
    // nothing else in the renderer holds one separately from the projection.
    JPH::Mat44    view_matrix    = JPH::Mat44::sIdentity();
    JPH::Mat44    shadowProjView = JPH::Mat44::sIdentity();
    FrameUniforms currentUniforms {};
    float         currentDt = 0.0166f;

    GraphicsSettings settings {};

    FrameProfiler      gpuProfiler;
    Vk::GPUDiagnostics gpuDiagnostics;

    PipelineRegistry pipelines;

    GpuPipelineCounters pendingPipelineCounters {};

    ZHLN::Optional<FS::FileSystemWatcher&> fileSystemWatcher;
    FS::FileWatchHandle                    shaderDirectoryWatch = 0;
    ShaderReloadRegistry                   shaderReloads;

    uint32_t nextMorphDeltaIndex = 0;
    uint32_t blueNoiseTexIdx     = 0;

    float lastAspectRatio = 0.0f;
    float lastFov         = 0.0f;
    float lastNearZ       = 0.0f;
    float lastFarZ        = 0.0f;

    ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;

    ReflectionPipeline reflectionPipeline;

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const noexcept {
        gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
    void RegisterShader(const Vk::ShaderDesc& desc, std::string_view fallbackEntry = "main") const noexcept {
        gpuDiagnostics.RegisterShader(desc, fallbackEntry);
    }

    Impl(PresentationTarget& target, ZHLN::Optional<FS::FileSystemWatcher&> watcher):
        presentationTarget(target), targets(ctx, allocator, graphicsCmdRing), textureManager(ctx, allocator, stagingRingBuffer, graphicsCmdRing, heapManager),
        geometry(ctx, allocator, transferRingBuffer, transferCmdRing, deletionQueue),
        pipelines(ctx, pipelineCache, sceneHeapMappings, gpuDiagnostics, deletionQueue, emptyPipelineLayout), fileSystemWatcher(watcher),
        reflectionPipeline(*this) {
    }

    ~Impl() {
        // Also runs when initialization fails partway through, before a
        // RenderContext has been constructed. All explicit VMA destruction
        // below precedes the allocator member's destructor.
        if (ctx.Device() != VK_NULL_HANDLE) {
            if (auto waited = Vk::WaitIdle(ctx.Device()); !waited) {
                ZHLN::LogError("[Render] device idle wait during teardown failed: {}", waited.error());
            }
        }
        submittedStaging.reset();
        DestroyDestinations();
        frameOpen = false;
        if (fileSystemWatcher && shaderDirectoryWatch != 0) {
            static_cast<void>(fileSystemWatcher->Unwatch(shaderDirectoryWatch));
        }

        ClearEmitterBuffers();
        geometry.RetireAll();
        // The registry destructor runs after this body; enqueue its pipelines
        // now, before the explicit deletion-queue drain below.
        pipelines.RetireAll();
        // Acceleration structures must go before their backing VMA buffers.
        for (auto& tlas: frames.tlas) {
            tlas = Vk::AccelerationStructure {};
        }
        uiRenderer.DestroyBuffers(allocator);
        shadows.DestroyResources(allocator);
        fog.DestroyNoise(allocator);
        textureManager.Reset();
        iblPayload.Destroy(allocator);
        ltcMatView = {};
        ltcAmpView = {};
        allocator.DestroyImage(ltcMatImage);
        allocator.DestroyImage(ltcAmpImage);
        targets.Clear();
        for (auto& history: accumulationHistory) {
            history.Destroy(allocator);
        }
        presenter.Cleanup();

        auto destroyFrames = [this](auto& buffers) {
            for (auto& buffer: buffers)
                allocator.DestroyBuffer(buffer);
        };
        destroyFrames(frames.lineVbos);
        destroyFrames(frames.clusterGridBuffers);
        destroyFrames(frames.lightIndexListBuffers);
        destroyFrames(frames.globalCounterBuffers);
        destroyFrames(frames.frameUniformBuffers);
        destroyFrames(frames.lightStorageBuffers);
        destroyFrames(frames.instanceDataBuffers);
        destroyFrames(frames.indirectCommandsBuffers);
        destroyFrames(frames.indirectCommandsBuffersPass2);
        destroyFrames(frames.secondPassCandidatesBuffers);
        destroyFrames(frames.secondPassCountBuffers);
        destroyFrames(frames.jointBuffers);
        destroyFrames(frames.tlasBuffer);
        destroyFrames(frames.tlasScratchBuffer);
        destroyFrames(frames.tlasInstanceBuffers);
        destroyFrames(frames.fogVolumesBuffer);
        allocator.DestroyBuffer(clusterBoundsBuffer);
        allocator.DestroyBuffer(morphDeltasBuffer);

        deletionQueue.Drain();
        graphicsCmdRing.Cleanup();
        transferCmdRing.Cleanup();
    }

    [[nodiscard]] std::expected<void, ErrorCode> InitSubsystems(const RenderConfig& cfg, int width, int height);
    [[nodiscard]] std::expected<void, ErrorCode> InitDiagnosticsAndProfiling();
    [[nodiscard]] std::expected<void, ErrorCode> InitCorePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> InitParallelRecorders();
    [[nodiscard]] std::expected<void, ErrorCode> BuildSpecializedLightingPipelines();

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
        JPH::Float4     baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Float4     emissiveFactor {0.0f, 0.0f, 0.0f, 1.0f};
        VkDeviceAddress tangentFrameAddress;
        VkDeviceAddress surfaceAddress;
        VkDeviceAddress iboAddress;

        uint32_t indexCount;
        uint32_t albedoIdx;
        uint32_t normalIdx;
        uint32_t pbrIdx;
        uint32_t emissiveIdx;
        float    roughness;
        float    metallic;
        float    alphaCutoff;
        uint32_t alphaMode;

        uint32_t samplerCodes0;
        uint32_t samplerCodes1;
        uint32_t unlit;
    };
    static_assert(sizeof(MeshParticleRenderPush) == 120);
    static_assert(offsetof(MeshParticleRenderPush, baseColorFactor) == 16);
    static_assert(offsetof(MeshParticleRenderPush, emissiveFactor) == 32);
    static_assert(offsetof(MeshParticleRenderPush, tangentFrameAddress) == 48);
    static_assert(offsetof(MeshParticleRenderPush, surfaceAddress) == 56);
    static_assert(offsetof(MeshParticleRenderPush, indexCount) == 72);

    struct ObjectConstants {
        uint32_t instanceId;
        uint32_t isShadowPass;
    };
    static_assert(sizeof(ObjectConstants) == 8);

    struct UIObjectConstants {
        JPH::Mat44 orthoMatrix;
        uint64_t   posAddress;
        uint64_t   surfaceAddress;
        uint32_t   albedoIdx;
        uint32_t   isSDF;
        uint32_t   useTextureColor;
    };
    static_assert(sizeof(UIObjectConstants) == 96);
    static_assert(offsetof(UIObjectConstants, surfaceAddress) == 72);

    using ScenePassPushConstants = GeneratedGpu::ScenePassPushConstants;
    using PPPushConstants        = ScenePassPushConstants;

    struct alignas(8) SkinningConstants {
        VkDeviceAddress inPosAddr;
        VkDeviceAddress inFrameAddr;
        VkDeviceAddress inSkinAddr;
        VkDeviceAddress outPosAddr;
        VkDeviceAddress outFrameAddr;
        VkDeviceAddress jointsAddr;
        VkDeviceAddress morphDeltasAddr;
        uint32_t        vertexCount;
        uint32_t        jointOffset;
        uint32_t        morphOffset;
        uint32_t        activeMorphCount;
        JPH::Float4     morphWeights {0.0f, 0.0f, 0.0f, 0.0f};
    };
    static_assert(offsetof(SkinningConstants, inFrameAddr) == 8);
    static_assert(offsetof(SkinningConstants, outFrameAddr) == 32);

    struct BakePush {
        uint32_t width;
        uint32_t height;
        float    scale;
        float    randomness;
        float    distortion;
    };

    // The payloads still declared here are the ones with no single pass to
    // live in: skinning, baking and the shared instance-draw constants are
    // reached from more than one place. Every payload that belongs to exactly
    // one pass is declared next to that pass and asserted there.
    static_assert(
        (GpuAbi::ScenePassPayload<ComputePushConstants> && GpuAbi::ScenePassPayload<ParticleRenderPushConstants> &&
         GpuAbi::ScenePassPayload<MeshParticleComputePush> && GpuAbi::ScenePassPayload<MeshParticleRenderPush> && GpuAbi::ScenePassPayload<ObjectConstants> &&
         GpuAbi::ScenePassPayload<UIObjectConstants> && GpuAbi::ScenePassPayload<SkinningConstants> && GpuAbi::ScenePassPayload<BakePush> &&
         GpuAbi::ScenePassPayload<ScenePassPushConstants>),
        "a shared pass payload no longer fits the push blob's prefix in front of the frame addresses"
    );

    void UploadSubmittedLights() noexcept;

    void ProvokeDeviceLostInternal() const;

    [[nodiscard]] std::expected<void, ErrorCode> BuildSkinningPipeline();
    void                                         DispatchSkinningPasses(VkCommandBuffer cmd);

    [[nodiscard]] std::expected<void, ErrorCode> BuildProceduralBakePipeline();
    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
        -> std::expected<TextureHandle, ErrorCode>;

    void BuildTLAS(VkCommandBuffer cmd) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> InitShadowResources();
    [[nodiscard]] std::expected<void, ErrorCode> InitCullingResources();
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
    [[nodiscard]] std::expected<void, ErrorCode> BuildHangGpuPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> InitPostProcessing();
    [[nodiscard]] std::expected<void, ErrorCode> InitCSGPipelines();
    [[nodiscard]] std::expected<void, ErrorCode> SetupUI();
    [[nodiscard]] std::expected<void, ErrorCode> BuildHiZPipeline();

    void BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh);

    [[nodiscard]] auto InitializeSystemTextures() noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto InitializeVolumetricNoiseTexture() noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto InitializeBlueNoiseTexture() -> std::expected<void, ErrorCode>;

    // The scene frame is assembled by the graph builder in
    // RenderGraphBuilder.cpp; the compute frame lives in
    // pipelines/ComputeSimPipeline.cpp next to the queue it is submitted on.
    void RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& settings);

    void BeginShaderObservation();
    void HandleShaderFileEvent(const FS::FileWatchEvent& event);

    [[nodiscard]] std::expected<void, ErrorCode> RecreateTargets(VkExtent2D ext);

    void ApplySettings(GraphicsSettings incoming) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> InitSkeletalAnimationResources();
    [[nodiscard]] std::expected<void, ErrorCode> InitLightingLUTs();

    [[nodiscard]] std::expected<Vk::OwnedShaderStages, ErrorCode> LoadAndCreateShaders(Vk::VertexStageSource vs, Vk::FragmentStageSource ps) const noexcept;
    template <Vk::ShaderProgram ShaderModule, typename PushConstants = void>
    [[nodiscard]] auto LoadAndCreateComputeShader(Vk::ComputeStageSource cs, VkPipelineLayout layout, Vk::DynamicComputePass& pass) const noexcept
        -> std::expected<Vk::Pipeline, ErrorCode>;

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ctx.BufferAddress(buffer);
    }
};

template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
auto RenderContext::Impl::BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
    -> std::expected<uint32_t, ErrorCode> {
    static_assert(Vk::GpuTriviallyCopyable<PushT>);
    const auto imageConfig = Vk::ImageConfig::Texture2D({width, height}, format, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled);
    return Vk::Image::Create(allocator, imageConfig).and_then([&](Vk::Image image) -> std::expected<uint32_t, ErrorCode> {
        defer _([&] { allocator.DestroyImage(image); });
        auto  viewRes = image.CreateView(ctx.Device(), {.kind = Vk::ImageViewKind::Texture2D});
        if (!viewRes) {
            return std::unexpected(viewRes.error());
        }
        Vk::ImageView view = std::move(*viewRes);
        heapManager.BeginImmediate();
        const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Declared>(ctx, bakeHeapBindings, Vk::Slot<"outTexture">(view));

        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
            heapManager.BindHeaps(cmd);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, image.Handle());
            pass.DispatchHeapIndexedThreads<Modules...>(ctx, cmd, block, width, height, 1, push);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
        });
        return textureManager.Adopt(std::move(image), std::move(view));
    });
}

// Pass-local encoder and renderer state for the draw helpers. The encoder
// holds the sole command-buffer handle; the frame slot comes from ctx.presenter.
struct PassContext {
    Vk::CommandEncoder   encoder;
    RenderContext::Impl& ctx;
    bool                 heapsInherited;

    PassContext(VkCommandBuffer cmd, RenderContext::Impl& impl, bool inherited = false) noexcept: encoder(cmd), ctx(impl), heapsInherited(inherited) {
    }

    [[nodiscard]] auto Cmd() const noexcept -> VkCommandBuffer {
        return encoder.cmd;
    }

    void EnsureHeapState() noexcept {
        if (!heapsInherited) {
            ctx.BindHeapsAndPushFrame(Cmd());
        }
    }
};

struct GroupRange {
    const NativeMaterial* material;
    uint32_t              start;
    uint32_t              count;
    VkCullModeFlags       cullMode;
};

inline std::vector<uint32_t> LoadShaderSpv(const std::string& path) noexcept {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    const auto fileSize = static_cast<std::streamoff>(file.tellg());
    // A partial word would make read() overrun the uint32_t buffer and cannot
    // be passed to VkShaderModuleCreateInfo in any case.
    if (fileSize <= 0 || fileSize % sizeof(uint32_t) != 0) {
        return {};
    }
    std::vector<uint32_t> buffer(static_cast<size_t>(fileSize) / sizeof(uint32_t));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize))) {
        return {};
    }
    return buffer;
}

// Embedded shaders are byte arrays, not uint32_t objects. ShaderBytecode
// keeps a byte span until CreateShaderDesc crosses the C ABI boundary.
template <VkShaderStageFlagBits Stage>
[[nodiscard]] inline auto LoadShaderData(const Vk::ShaderStageSource<Stage>& src) -> Vk::ShaderBytecode {
    if constexpr (isDev) {
        auto disk = LoadShaderSpv(src.path);
        if (!disk.empty()) {
            return {.storage = std::move(disk)};
        }
    }

    const auto bytes = std::as_bytes(src.fallback);
    if (bytes.empty() || bytes.size() % sizeof(uint32_t) != 0) {
        return {};
    }
    // Generated fallbacks are word-aligned. A caller-supplied byte span need
    // not be: copy only that case before the C ABI reads uint32_t words.
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(uint32_t) != 0) {
        std::vector<uint32_t> aligned(bytes.size() / sizeof(uint32_t));
        std::ranges::copy(bytes, std::as_writable_bytes(std::span {aligned}).begin());
        return {.storage = std::move(aligned)};
    }
    return {.fallback = bytes};
}

template <Vk::ShaderProgram ShaderModule, typename PushConstants>
auto RenderContext::Impl::LoadAndCreateComputeShader(Vk::ComputeStageSource cs, VkPipelineLayout layout, Vk::DynamicComputePass& pass) const noexcept
    -> std::expected<Vk::Pipeline, ErrorCode> {
    const auto           loaded = LoadShaderData(cs);
    const Vk::ShaderDesc shader = Vk::CreateShaderDesc(loaded.Code(), cs.entryPoint);
    gpuDiagnostics.RegisterShader(shader, "CSMain");
    if (shader.code == nullptr || shader.size == 0) {
        return std::unexpected(Vk::ShaderStageCreationError::ShaderLoadingFailed);
    }
    if (!pass.ReflectDispatchLayout(shader)) {
        return std::unexpected(Vk::SpirvLayoutError::ModuleParseFailed);
    }

    return Vk::ComputePipeline<ShaderModule, PushConstants>::Create(ctx, shader, Vk::PipelineCreateBindings {.layout = layout}, pipelineCache.Get());
}

template <typename T = Vk::Buffer, typename... Args>
[[nodiscard]] auto CreatePerFrame(Vk::Allocator& alloc, const Args&... args) -> std::expected<PerFrame<T>, ErrorCode> {
    static_assert(std::is_same_v<T, Vk::Buffer>, "VMA per-frame resources need an explicit cleanup policy");
    PerFrame<T> resources;
    defer       _([&] {
        for (auto& resource: resources)
            alloc.DestroyBuffer(resource);
    });
    for (auto& resource: resources) {
        auto created = T::Create(alloc, args...);
        if (!created) {
            return std::unexpected(created.error());
        }
        resource = std::move(*created);
    }
    return std::expected<PerFrame<T>, ErrorCode> {std::move(resources)};
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

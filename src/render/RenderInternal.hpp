// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "DrawCommands.hpp"
#include "DrawQueueManager.hpp"
#include "FrameContext.hpp"
#include "FrameDestinations.hpp"
#include "FrameResources.hpp"
#include "GenerationalPool.hpp"
#include "GeometryManager.hpp"
#include "GpuAbi.hpp"
#include "GpuLayout.hpp"
#include "PassPayloads.hpp"
#include "PipelineFormats.hpp"
#include "PipelineRegistry.hpp"
#include "PresentationTarget.hpp"
#include "RenderConstants.hpp"
#include "Rendering.hpp"
#include "ShaderReloadRegistry.hpp"
#include "TargetManager.hpp"
#include "TextureManager.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "features/PostProcessFeature.hpp"
#include "features/ShadowAtlas.hpp"
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

namespace ZHLN {

[[nodiscard]] inline auto AsFrameError(ErrorCode code) noexcept -> ErrorCode {
    return Vk::IsDeviceLost(code) ? ErrorCode {FrameResult::DeviceLost} : code;
}

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept;

namespace Diag {
[[nodiscard]] bool DisableGpuCulling() noexcept;
[[nodiscard]] bool ForkSequentialForced() noexcept;
} // namespace Diag

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

// Everything not listed here is on RenderFrameContext; see FrameContext.hpp.
// The base is what a pass sees, and RenderContext::Impl is what owns the rest
// of it: initialization, presentation internals, diagnostics, the host-side
// resource registries, and the pipelines that still belong to a pass rather
// than to the frame.
struct RenderContext::Impl : RenderFrameContext {
    struct RenderState {
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> initialState;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           finalColor;
        Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>                                           bloomFinal;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> resourcesForAA;
        SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> aaResult;
    };

    static constexpr uint32_t SHADOW_RES   = TargetManager::kShadowResolution;
    static constexpr uint32_t NUM_CASCADES = TargetManager::kCascades;

    static constexpr uint32_t kMaxLineVertices               = 500'000;
    static constexpr uint32_t kMaxDebugVertices              = 500'000;
    static constexpr uint32_t kGpuCullingMaxInstances        = 8'192;
    static constexpr uint32_t kGpuCullingMaxBatches          = 256;
    static constexpr uint32_t kGpuCullingMaxVisibleInstances = kGpuCullingMaxInstances * kGpuCullingMaxBatches;

    PresentationTarget&                                           presentationTarget;
    String64                                                      appName;
    Vk::PipelineBinaryStore                                       pipelineBinaries;
    PresentationMode                                              presentationMode = PresentationMode::NativeSwapchain;
    Vk::CommandPools<Vk::kFramesInFlight, Vk::QueueType::Compute> computePools;
    mutable Vk::CommandRing<Vk::QueueType::Compute, 8>  computeCmdRing;

    std::unique_ptr<Vk::SubmittedStagingWork> submittedStaging;
    bool                                      frameOpen = false;

    PerFrame<Vk::ParallelCommandRecorder<kParallelRecordingSlots>> parallelRecorders;

    Vk::ReflectedLayout bindlessLayout;

    Vk::HeapMappingBundle sceneHeapMappings;
    Vk::HeapMappingBundle decalSceneHeapMappings;
    Vk::HeapMappingBundle decalHeapMappings;

    Vk::HeapPassBindings hizHeapBindings;
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
    Vk::DynamicComputePass skinningPass;
    Vk::DynamicComputePass proceduralBakePass;
    Vk::DynamicComputePass hangGpuPass;

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
    Vk::TypedPipeline<1, false> particleRenderPipeline;

    Vk::DynamicComputePass meshParticleUpdatePass;

    Vk::ReflectedLayout decalDescLayout;
    Vk::Pipeline        decalPipeline;

    Vk::Pipeline     linePipeline;
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

    Vk::Image     ltcMatImage;
    VkImageViewCreateInfo ltcMatInfo {};
    Vk::Image     ltcAmpImage;
    VkImageViewCreateInfo ltcAmpInfo {};

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

    // The shadow atlas's answer to this frame's lights: one tile base per
    // submitted light, in submitted order, -1 for the lights that asked and did
    // not fit (and the ones that never asked). `UploadSubmittedLights` fills it
    // before it packs, so the packed lights carry the atlas's decision.
    ZHLN::Array<int32_t> lightTileBases;

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

    UIRenderer uiRenderer;

    struct RenderTexture {
        Vk::ImageSlice       image {};
        // Owns the attachment handle `image` borrows; the descriptor side of
        // this texture needs no handle (TextureManager stores a create-info).
        Vk::AttachmentView   attachmentView;
        Vk::AttachmentLayout layout        = Vk::AttachmentLayout::Undefined;
        bool                 drawn         = false;
    };
    std::unordered_map<TextureHandle, RenderTexture> renderTextures;

    // A process-wide renderer ID prevents stale frame capabilities from
    // aliasing a new renderer after device-loss recovery.
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

    [[nodiscard]] auto ForkExecutor() noexcept -> ForkReplayer* {
        return forkReplayer.get();
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
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;
    [[nodiscard]] auto EnsureEmitterStorage(EmitterStorageMap& emitters, uint64_t emitterId, uint32_t maxParticles, size_t particleStride)
        -> BufferHandle;
    void EvictInactiveEmitters() noexcept;
    void ClearEmitterBuffers() noexcept;

    [[nodiscard]] auto PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal>;

    // Non-owning view while a scene graph is executing. Never cached across
    // a frame or used as an alternate path around ResolveTarget.
    std::optional<Vk::ImageSlice> sceneTarget;

    void ApplySceneView(const SceneView& view) noexcept;

    void PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept;

    FrameProfiler      gpuProfiler;
    Vk::GPUDiagnostics gpuDiagnostics;

    PipelineRegistry pipelines;

    GpuPipelineCounters pendingPipelineCounters {};

    ZHLN::Optional<FS::FileSystemWatcher&> fileSystemWatcher;
    FS::FileWatchHandle                    shaderDirectoryWatch = 0;
    ShaderReloadRegistry                   shaderReloads;

    uint32_t nextMorphDeltaIndex = 0;

    float lastAspectRatio = 0.0f;
    float lastFov         = 0.0f;
    float lastNearZ       = 0.0f;
    float lastFarZ        = 0.0f;

    ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const noexcept {
        gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
    void RegisterShader(const Vk::ShaderDesc& desc, std::string_view fallbackEntry = "main") const noexcept {
        gpuDiagnostics.RegisterShader(desc, fallbackEntry);
    }

    // RenderFrameContext's own constructor builds targets, textureManager,
    // geometry and reflectionPipeline; they depend on the device members the
    // base declares, which is why they cannot be initialized here.
    Impl(PresentationTarget& target, ZHLN::Optional<FS::FileSystemWatcher&> watcher):
        presentationTarget(target), pipelines(ctx, pipelineBinaries, sceneHeapMappings, gpuDiagnostics, deletionQueue), fileSystemWatcher(watcher) {
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
        uiRenderer.DestroyBuffers(allocator);
        shadows.DestroyResources(allocator);
        fog.DestroyNoise(allocator);
        textureManager.Reset();
        iblPayload.Destroy(allocator);
        ltcMatInfo = {};
        ltcAmpInfo = {};
        allocator.DestroyImage(ltcMatImage);
        allocator.DestroyImage(ltcAmpImage);
        targets.Clear();
        for (auto& history: accumulationHistory) {
            history.Destroy(allocator);
        }
        presenter.Cleanup();

        // Every per-frame allocation, released in the order the driver needs:
        // acceleration structures first, then their backing VMA buffers, then
        // the rest. The list lives in FrameResources, next to the members.
        frames.Destroy(allocator);
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

    // Packs the submitted lights, and answers their shadow asks first: the
    // atlas allocation has to exist before the pack, because a light's tile
    // base is one of the fields the shader reads.
    void UploadSubmittedLights(const Camera& cam) noexcept;
    void AllocateShadowTiles(const JPH::Vec3& viewPos) noexcept;

    void ProvokeDeviceLostInternal() const;

    [[nodiscard]] std::expected<void, ErrorCode> BuildSkinningPipeline();
    // Skins every queued skinned draw, then rebuilds each scratch buffer's
    // BLAS. The returned entries are the only channel from the rebuild to
    // BuildTLAS: a scratch buffer whose rebuild failed is simply absent
    // (and the failure is handed back alongside, never swallowed).
    struct SkinnedBlasOutcome {
        ZHLN::Array<SkinnedBlasEntry> addresses;
        std::optional<ErrorCode>      failure;
    };
    [[nodiscard]] auto DispatchSkinningPasses(VkCommandBuffer cmd) -> SkinnedBlasOutcome;

    [[nodiscard]] std::expected<void, ErrorCode> BuildProceduralBakePipeline();
    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
        -> std::expected<TextureHandle, ErrorCode>;

    void BuildTLAS(VkCommandBuffer cmd, std::span<const SkinnedBlasEntry> skinnedBlas) noexcept;

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
};

template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
auto RenderContext::Impl::BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
    -> std::expected<uint32_t, ErrorCode> {
    static_assert(Vk::GpuTriviallyCopyable<PushT>);
    const auto imageConfig = Vk::ImageConfig::Texture2D({width, height}, format, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled);
    return Vk::Image::Create(allocator, imageConfig).and_then([&](Vk::Image image) -> std::expected<uint32_t, ErrorCode> {
        defer _([&] { allocator.DestroyImage(image); });
        const VkImageViewCreateInfo viewInfo = image.ViewInfo();
        heapManager.BeginImmediate();
        const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Declared>(bakeHeapBindings, Vk::Slot<"outTexture">(viewInfo));

        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
            heapManager.BindHeaps(cmd);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, image.Handle());
            pass.DispatchHeapIndexedThreads<Modules...>(cmd, block, width, height, 1, push);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
        });
        return textureManager.Adopt(std::move(image), viewInfo);
    });
}

// Pass-local encoder and renderer state for the draw helpers. The encoder
// holds the sole command-buffer handle; the frame slot comes from ctx.presenter.
struct PassContext {
    Vk::CommandEncoder   encoder;
    RenderFrameContext&  ctx;
    bool                 heapsInherited;

    PassContext(VkCommandBuffer cmd, RenderFrameContext& impl, bool inherited = false) noexcept: encoder(cmd), ctx(impl), heapsInherited(inherited) {
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

inline std::vector<uint32_t> LoadShaderSpv(std::string_view path) noexcept {
    std::ifstream file(std::filesystem::path(path), std::ios::ate | std::ios::binary);
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

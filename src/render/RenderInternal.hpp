// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "ComputePassResources.hpp"
#include "DrawCommands.hpp"
#include "FrameContext.hpp"
#include "FrameDestinations.hpp"
#include "FrameResources.hpp"
#include "GeometryManager.hpp"
#include "GpuLayout.hpp"
#include "PipelineRegistry.hpp"
#include "PresentationTarget.hpp"
#include "RenderDiagnostics.hpp"
#include "Rendering.hpp"
#include "TargetManager.hpp"
#include "TextureManager.hpp"
#include "Zahlen/Core/BatchResult.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "features/ShadowRenderer.hpp"
#include "features/VolumetricFogSystem.hpp"
#include "graph/RenderPassDecl.hpp"
#include "passes/SceneDrawResources.hpp"
#include "passes/aa/AntiAliasingResources.hpp"
#include "passes/culling/CullingResources.hpp"
#include "passes/forward/ForwardResources.hpp"
#include "passes/gbuffer/DecalResources.hpp"
#include "passes/postprocess/PostProcessResources.hpp"
#include "passes/simulation/ParticleUpdateResources.hpp"
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
#include <Zahlen/Error.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Vertex.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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

using GlobalSceneLayout    = Vk::ReflectedLayout;
using CullingLayout        = Vk::ReflectedLayout;
using HiZGenerateLayout    = Vk::ReflectedLayout;
using ClusterCullingLayout = Vk::ReflectedLayout;
using BakeLayout           = Vk::ReflectedLayout;
using DecalLayout          = Vk::ReflectedLayout;

namespace Resource {
}

struct RenderContext::Impl: RenderFrameContext {
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
    mutable Vk::CommandRing<Vk::QueueType::Compute, 8>            computeCmdRing;

    std::unique_ptr<Vk::SubmittedStagingWork> submittedStaging;
    bool                                      frameOpen = false;

    PerFrame<Vk::ParallelCommandRecorder<kParallelRecordingSlots>> parallelRecorders;

    Vk::ReflectedLayout bindlessLayout;

    Vk::HeapMappingTable sceneHeapMappings;
    // GlobalSceneRegistry at Set 1 (see BuildSceneHeapMappings): composed into
    // any heap pass whose module references `scene` at Set 1 (decal, sky fill).
    Vk::HeapMappingTable sceneSet1HeapMappings;
    Vk::HeapMappingTable decalHeapMappings;

    // Depth reduction, the GPU draw cull, and the two cluster passes: one
    // family, owned by the passes that dispatch them. Takes the place of the
    // three heap-binding slots that used to sit here, so construction and
    // destruction order relative to its neighbours is unchanged.
    CullingResources culling;

    // The pipelines the shared SceneDraw helpers draw with: the stencil trio
    // for CSG, the pair for mesh particles.
    CsgResources          csg;
    MeshParticlePipelines meshParticles;
    Vk::HeapPassBindings  bakeHeapBindings;
    Vk::HeapPassBindings  iblBakeHeapBindings;
    Vk::HeapPassBindings  volumetricClearHeapBindings;
    Vk::HeapPassBindings  volumetricFogInjectHeapBindings;
    Vk::HeapPassBindings  volumetricLightInjectHeapBindings;
    Vk::HeapPassBindings  volumetricIntegrationHeapBindings;
    Vk::HeapPassBindings  volumetricTemporalHeapBindings;

    Vk::Sampler globalSampler;
    Vk::Sampler clampSampler = Vk::Sampler::LinearClampToEdge();
    Vk::Sampler shadowSampler;
    Vk::Sampler defaultSampler;
    Vk::Sampler pointSampler;
    Vk::Sampler blueNoiseSampler;

    Vk::SamplerHandle globalSamplerSlot;
    Vk::SamplerHandle materialSamplerBaseSlot; // Nine contiguous S/T wrap combinations.
    Vk::SamplerHandle clampSamplerSlot;
    Vk::SamplerHandle pointSamplerSlot;
    Vk::TextureHandle iblPrefilteredSlot;
    Vk::TextureHandle iblBrdfLutSlot;
    Vk::TextureHandle transLightingSlot;
    Vk::TextureHandle decalDepthSlot;

    // The six AA pipelines, owned as one group by the passes that read them.
    AntiAliasingResources antiAliasing;

    // The three standalone compute passes, dispatched from Impl itself.
    ComputePassResources compute;

    struct EmitterStorage {
        BufferHandle buffer                  = BufferHandle::Invalid;
        uint32_t     capacity                = 0;
        uint64_t     lastSeenFrame           = 0;
        bool         allocationWarningLogged = false;
    };
    using EmitterStorageMap = HashMap<uint64_t, EmitterStorage>;
    EmitterStorageMap particleEmitters;
    EmitterStorageMap meshParticleEmitters;

    // The emitter-advance pipelines: billboard and mesh particle simulation.
    ParticleUpdateResources particleUpdate;

    // Decals, and the forward pass's lines and billboard particles.
    DecalResources               decals;
    ForwardResources             forward;
    std::span<const LineSegment> pendingLines;

    std::expected<void, ErrorCode> BuildLinePipeline();
    std::expected<void, ErrorCode> InitLineBuffers() noexcept;

    std::expected<void, ErrorCode> AllocateLineVertexBuffers(size_t maxVertices, PerFrame<Vk::Buffer>& bufs, PerFrame<VkDeviceAddress>& addrs) noexcept;

    std::expected<void, ErrorCode>               InitSceneHeaps(Vk::Sampler globalSamplerValue, Vk::Sampler clampSamplerValue) noexcept;
    void                                         BuildSceneHeapMappings() noexcept;
    void                                         BuildDecalHeapMappings() noexcept;
    void                                         WriteSceneStaticImageDescriptors() noexcept;
    void                                         WritePointSamplerToHeap(Vk::Sampler pointSamplerValue) noexcept;
    void                                         WriteTransLightingToHeap() noexcept;
    void                                         InitPassSamplerDescriptors() noexcept;
    [[nodiscard]] std::expected<void, ErrorCode> InitBakeHeapBindings() noexcept;
    template <typename Declared, Vk::ShaderProgram... Modules, typename PushT>
    [[nodiscard]] auto BakeComputeTexture2D(const Vk::DynamicComputePass& pass, uint32_t width, uint32_t height, VkFormat format, const PushT& push)
        -> std::expected<uint32_t, ErrorCode>;

    Vk::ReflectedLayout proceduralBakeDescLayout;
    Vk::ReflectedLayout iblBakeDescLayout;

    Vk::Image ltcMatImage;
    Vk::Image ltcAmpImage;

    struct BillboardSlot {
        uint32_t               textureIndex = 0;
        uint32_t               blendMode    = 0;
        uint32_t               alignment    = 0;
        uint32_t               serial       = 0;
        PerFrame<BufferHandle> buffers {};
        PerFrame<uint32_t>     capacities {};
    };
    ZHLN::Array<BillboardSlot> billboardSlots;
    ZHLN::Array<Particle>      billboardStaging;
    uint32_t                   billboardSerial = 0;

    ZHLN::Array<int32_t> lightTileBases;

    ZHLN::Array<Light> gpuLights;

    ZHLN::Array<LightDesc> submittedLights;

    uint32_t packedLightCount = 0;

    UIRenderer uiRenderer;

    struct RenderTexture {
        Vk::ImageSlice image;

        Vk::AttachmentView   attachmentView;
        Vk::AttachmentLayout layout = Vk::AttachmentLayout::Undefined;
        bool                 drawn  = false;
    };
    std::unordered_map<TextureHandle, RenderTexture> renderTextures;

    static inline std::atomic<uint64_t> nextRendererId {1};
    uint64_t                            rendererId      = nextRendererId.fetch_add(1, std::memory_order::relaxed);
    uint64_t                            frameSerial     = 0;
    uint64_t                            nextAcquisition = 1;
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

    [[nodiscard]] auto ForkExecutor() const noexcept -> ForkReplayer* {
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
    [[nodiscard]] auto EnsureEmitterStorage(EmitterStorageMap& emitters, uint64_t emitterId, uint32_t maxParticles, size_t particleStride) -> BufferHandle;
    void               EvictInactiveEmitters() noexcept;
    void               ClearEmitterBuffers() noexcept;

    [[nodiscard]] auto PresentUsedWindows() noexcept -> PresentBatch;

    std::optional<Vk::ImageSlice> sceneTarget;

    void ApplySceneView(const SceneView& view) noexcept;

    void PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept;

    RenderDiagnostics diagnostics;
    std::atomic<bool> deviceLost {false};

    void NoteDeviceLost(std::string_view site) noexcept {
        if (!deviceLost.exchange(true, std::memory_order::relaxed)) {
            ZHLN::LogError("[DeviceLost] Vulkan device lost ({})", site);
        }
    }

    PipelineRegistry pipelines;

    uint32_t nextMorphDeltaIndex = 0;

    float lastAspectRatio = 0.0f;
    float lastFov         = 0.0f;
    float lastNearZ       = 0.0f;
    float lastFarZ        = 0.0f;

    ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;

    void WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const noexcept {
        diagnostics.gpuDiagnostics.WriteCheckpoint(cmd, name);
    }

    explicit Impl(PresentationTarget& target):
        presentationTarget(target), diagnostics(ctx), pipelines(ctx, heapManager, pipelineBinaries, sceneHeapMappings, deletionQueue) {
    }

    ~Impl() {
        if (ctx.Device() != VK_NULL_HANDLE && !deviceLost.load(std::memory_order::relaxed)) {
            if (auto waited = Vk::WaitIdle(ctx.Device()); !waited) {
                if (Vk::IsDeviceLost(waited.error())) {
                    NoteDeviceLost("RenderContext::~Impl");
                } else {
                    ZHLN::LogError("[Render] device idle wait during teardown failed: {}", waited.error());
                }
            }
        }
        submittedStaging.reset();
        DestroyDestinations();
        frameOpen = false;

        ClearEmitterBuffers();
        geometry.RetireAll();

        pipelines.RetireAll();
        uiRenderer.DestroyBuffers(allocator);
        shadows.DestroyResources(allocator);
        fog.DestroyNoise(allocator);
        textureManager.Reset();
        iblPayload.Destroy(allocator);
        post.ltcMat = {};
        post.ltcAmp = {};
        allocator.DestroyImage(ltcMatImage);
        allocator.DestroyImage(ltcAmpImage);
        targets.Clear();
        primaryDepthTarget.Destroy(allocator);
        for (auto& history: accumulationHistory) {
            history.Destroy(allocator);
        }
        presenter.Cleanup();

        frames.Destroy(allocator);
        allocator.DestroyBuffer(clusterBoundsBuffer);
        allocator.DestroyBuffer(morphDeltasBuffer);

        deletionQueue.Drain();
        allocator.DetachAddressTracker();
        diagnostics.gpuDiagnostics.Shutdown();
    }

    [[nodiscard]] std::expected<void, ErrorCode> InitSubsystems(const EngineConfig& cfg, int width, int height);
    [[nodiscard]] std::expected<void, ErrorCode> InitDiagnosticsAndProfiling();
    [[nodiscard]] std::expected<void, ErrorCode> InitCorePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> InitParallelRecorders();
    [[nodiscard]] std::expected<void, ErrorCode> BuildSpecializedLightingPipelines();

    void UploadSubmittedLights(const Camera& cam) noexcept;
    void AllocateShadowTiles(const JPH::Vec3& viewPos) noexcept;

    [[nodiscard]] auto ProvokeDeviceLostInternal() const -> std::expected<void, ErrorCode>;

    [[nodiscard]] std::expected<void, ErrorCode> BuildSkinningPipeline();

    [[nodiscard]] auto DispatchSkinningPasses(VkCommandBuffer cmd) -> BatchResult<SkinnedBlasEntry, ErrorCode>;

    [[nodiscard]] std::expected<void, ErrorCode> BuildProceduralBakePipeline();
    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
        -> std::expected<TextureHandle, ErrorCode>;

    void BuildTLAS(VkCommandBuffer cmd, std::span<const SkinnedBlasEntry> skinnedBlas) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> InitShadowResources();
    [[nodiscard]] std::expected<void, ErrorCode> InitCullingResources();
    [[nodiscard]] std::expected<void, ErrorCode> BuildDecalPipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildEmissivePipeline();
    [[nodiscard]] std::expected<void, ErrorCode> BuildParticlePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> BuildMeshParticlePipelines();
    [[nodiscard]] std::expected<void, ErrorCode> InitBindless();
    // Builds every LightingResources bundle: the GTAO, shadow, reflection and
    // GI dispatches and their denoisers. Splitting this from the bloom chain is
    // what lets each domain own its own heap rows.
    [[nodiscard]] std::expected<void, ErrorCode> BuildLightingResources();
    [[nodiscard]] std::expected<void, ErrorCode> BuildPostProcessResources();
    [[nodiscard]] std::expected<void, ErrorCode> BakeSMAALUTs();

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

    void RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd, const SceneView& view, const GraphicsSettings& settings);

    [[nodiscard]] std::expected<void, ErrorCode> RecreateTargets(VkExtent2D ext);
    [[nodiscard]] std::expected<void, ErrorCode> EnsureDepthTarget(FrameDestinations::Window& destination) noexcept;

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
        defer                   _([&] { allocator.DestroyImage(image); });
        const Vk::ImageView     view      = image.View();
        auto                    heapScope = heapManager.CreateImmediateScope();
        const Vk::HeapBlockBase block     = heapScope.WriteParameters<Declared>(bakeHeapBindings, Vk::Slot<"outTexture">(view));

        if (auto immediate = Vk::ExecuteImmediate(
                ctx, graphicsCmdRing,
                [&](VkCommandBuffer cmd) -> auto {
                    heapScope.Bind(cmd);
                    view.DiscardTo<VK_IMAGE_LAYOUT_GENERAL>(cmd);
                    pass.DispatchHeapIndexedThreads<Modules...>(cmd, block, width, height, 1, push);
                    view.Transition<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
                }
            );
            !immediate) [[unlikely]] {
            return std::unexpected(immediate.error());
        }
        return textureManager.Adopt(std::move(image), view);
    });
}

inline std::vector<uint32_t> LoadShaderSpv(std::string_view path) noexcept {
    std::ifstream file(std::filesystem::path(path), std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    const auto fileSize = static_cast<std::streamoff>(file.tellg());

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
        for (auto& resource: resources) {
            alloc.DestroyBuffer(resource);
        }
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
struct ZHLN::Vk::FormatOf<ZHLN::Packed1010102> {
    static constexpr auto value = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
};
template <>
struct ZHLN::Vk::FormatOf<ZHLN::PackedHalf2> {
    static constexpr auto value = VK_FORMAT_R16G16_SFLOAT;
};
template <>
struct ZHLN::Vk::FormatOf<ZHLN::PackedRGBA8> {
    static constexpr auto value = VK_FORMAT_R8G8B8A8_UNORM;
};

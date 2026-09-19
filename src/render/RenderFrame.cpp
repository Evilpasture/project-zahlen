// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "graph/RenderGraph.hpp"

#include <ShaderBindings.hpp> // Shaders::Modules::SkinningCS: the skinning push struct's module

#include "pipelines/ComputeSimPipeline.hpp"
#include "pipelines/DeferredPbrPipeline.hpp"
#include "pipelines/UIPipeline.hpp"
#include "Zahlen/Profiler.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace ZHLN {

namespace Diag {

auto DisableGpuCulling() noexcept -> bool {
    static const bool enabled = std::getenv("ZHLN_NO_GPU_CULLING") != nullptr;
    return enabled;
}

auto ForkSequentialForced() noexcept -> bool {
    static const bool enabled = std::getenv("ZHLN_FORK_SEQUENTIAL") != nullptr;
    return enabled;
}

} // namespace Diag

namespace {

template <typename... Ptrs>
[[nodiscard]] constexpr auto AnyNull(Ptrs... ptrs) noexcept -> bool {
    return (... || (ptrs == nullptr));
}

} // namespace

// ============================================================================
// RenderContext Infrastructure & Lifecycles
// ============================================================================

auto RenderContext::Impl::FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, Vk::kHeapFrameAddressCount> {
    // Order must match the PUSH_ADDRESS mapping offsets baked in
    // BuildSceneHeapMappings: {frame, lights, instances, joints, prevJoints, morphDeltas}.
    return {
        ctx.BufferAddress(frames.frameUniformBuffers[presenter.frameIndex].Handle()), ctx.BufferAddress(frames.lightStorageBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.instanceDataBuffers[presenter.frameIndex].Handle()), ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex ^ 1].Handle()),    ctx.BufferAddress(morphDeltasBuffer.Handle()),
    };
}

void RenderContext::Impl::BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept {
    // Legacy descriptor-set and push-constant commands elsewhere in the frame
    // invalidate heap + push-data state (and vice versa), so every heap-based
    // segment re-binds both heaps and re-pushes the per-frame device addresses
    // that back the scene registry's PUSH_ADDRESS mappings.
    heapManager.BindHeaps(cmd);
    const auto addresses = FrameHeapAddresses();
    Vk::PushHeapFrameAddresses(ctx, cmd, Vk::kHeapPushDataLayout, addresses);
}

auto RenderContext::GetFramebufferSize() const -> std::optional<Extent2D> {
    Extent2D size = _impl->window.GetSize();
    if (size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    return size;
}

void RenderContext::Impl::DispatchSkinningPasses(VkCommandBuffer cmd) {
    if (!hasSkinnedThisFrame || cmd == VK_NULL_HANDLE) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Compute Skinning");
    skinningPass.Bind(cmd);

    for (const auto& drawCmd: queues.drawQueue) {
        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            auto* posMesh     = drawCmd.posMesh;
            auto* attrMesh    = drawCmd.attrMesh;
            auto* skinMesh    = drawCmd.skinMesh;
            auto* scratchMesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer).value_or(nullptr);

            if (AnyNull(posMesh, attrMesh, scratchMesh)) {
                continue;
            }

            SkinningConstants pcs {
                .inPosAddr        = posMesh->vboAddress,
                .inAttrAddr       = attrMesh->vboAddress,
                .inSkinAddr       = (skinMesh != nullptr) ? skinMesh->vboAddress : 0,
                .outPosAddr       = scratchMesh->vboAddress,
                .outAttrAddr      = scratchMesh->vboAddress + (scratchMesh->vertexCount * sizeof(VertexPosition)),
                .jointsAddr       = ctx.BufferAddress(frames.jointBuffers->Handle()),
                .morphDeltasAddr  = ctx.BufferAddress(morphDeltasBuffer.Handle()),
                .vertexCount      = posMesh->vertexCount,
                .jointOffset      = drawCmd.jointOffset,
                .morphOffset      = drawCmd.morphOffset,
                .activeMorphCount = drawCmd.activeMorphCount,
                .morphWeights     = {drawCmd.morphWeights[0], drawCmd.morphWeights[1], drawCmd.morphWeights[2], drawCmd.morphWeights[3]}
            };

            skinningPass.PushConstants<Shaders::Modules::SkinningCS>(cmd, pcs);
            skinningPass.DispatchThreads(cmd, posMesh->vertexCount, 1, 1);
        }
    }

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite,
        Vk::BarrierStage::AccelerationStructureBuild | Vk::BarrierStage::Vertex,
        Vk::BarrierAccess::AccelerationStructureRead | Vk::BarrierAccess::ShaderRead
    );

    if (rtCtx.Valid()) {
        ZHLN::ScopedTimer profTimerBLAS("GPU Skinned BLAS Rebuilds");
        for (const auto& drawCmd: queues.drawQueue) {
            if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
                auto* scratchMesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer).value_or(nullptr);
                if (scratchMesh != nullptr) {
                    BuildOrUpdateSkinnedBLAS(cmd, drawCmd, scratchMesh);
                }
            }
        }

        Vk::MemoryBarrier(
            cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite,
            Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureRead
        );
    }
}

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd) noexcept {
    if (!rtCtx.Valid() || queues.drawQueue.empty()) {
        return;
    }

    tlasInstancesScratch.clear();
    tlasInstancesScratch.reserve(queues.drawQueue.size());

    using enum DrawFlags;

    for (uint32_t i = 0; i < queues.drawQueue.size(); ++i) {
        const auto& drawCmd = queues.drawQueue[i];
        auto*       mesh    = drawCmd.posMesh;

        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            mesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer).value_or(nullptr);
        }

        if (mesh == nullptr || mesh->blasAddress == 0 || ((drawCmd.flags & ExcludeFromTLAS) != None)) {
            continue;
        }

        const auto& t = drawCmd.instanceData.world;

        VkAccelerationStructureInstanceKHR inst {
            .transform = [&]() -> VkTransformMatrixKHR {
                VkTransformMatrixKHR m;
                for (int row = 0; row < 3; ++row) {
                    for (int col = 0; col < 4; ++col) {
                        m.matrix[row][col] = t(row, col);
                    }
                }
                return m;
            }(),
            .instanceCustomIndex                    = i,
            .mask                                   = 0xFF,
            .instanceShaderBindingTableRecordOffset = 0,
            .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
            .accelerationStructureReference         = mesh->blasAddress
        };

        tlasInstancesScratch.push_back(inst);
    }

    if (tlasInstancesScratch.empty()) {
        return;
    }

    auto& instanceBuf = frames.tlasInstanceBuffers[presenter.frameIndex];

    // The instance buffer is host-visible and coherent (CPU_TO_GPU): write it
    // directly while recording. The memcpy completes before submission, and
    // the double-buffered parity means the previous TLAS build against this
    // buffer finished frames ago -- no staging copy, no transfer barrier.
    std::memcpy(instanceBuf.Map().data, tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    ZHLN_TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    rtCtx.BuildTLAS(cmd, geom, frames.tlas[presenter.frameIndex], ctx.BufferAddress(frames.tlasScratchBuffer[presenter.frameIndex].Handle()), tlasInstancesScratch.size());

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite,
        Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead
    );
}

// ============================================================================
// View state
// ============================================================================

void RenderContext::Impl::ApplySceneView(const SceneView& view) noexcept {
    // The view's matrices are the rasterization matrices: the caller builds
    // them from the camera it renders with, which means they already carry the
    // TAA subpixel jitter when AA asks for it. The unjittered pair is the raw
    // product of that same view/projection -- it is what the reconstruction and
    // culling paths publish (depth -> world, the culling push constants), and it
    // is what the frame's FrameUniforms carried before this view was bound.
    const JPH::Mat44 unjittered = view.projMatrix * view.viewMatrix;

    current_view_proj    = view.viewProjMatrix;
    unjittered_view_proj = unjittered;

    currentUniforms.viewProj           = view.viewProjMatrix;
    currentUniforms.unjitteredViewProj = unjittered;
    currentUniforms.invViewProj        = unjittered.Inversed();
    currentUniforms.camPos[0]          = view.worldPosition.GetX();
    currentUniforms.camPos[1]          = view.worldPosition.GetY();
    currentUniforms.camPos[2]          = view.worldPosition.GetZ();
    currentUniforms.camPos[3]          = view.time;

    // Patch the live GPU slot: a full memcpy of currentUniforms would drop the
    // cascade matrices / SH / screen resolution that SetFrameData wrote. The
    // jittered/unjittered split matters here: `viewProj` is what the vertex
    // stage rasterizes with, `unjitteredViewProj` is what TAA-style reprojection
    // (and every depth -> world reconstruction) undoes the jitter with.
    auto  mapped = frames.frameUniformBuffers[presenter.frameIndex].Map();
    auto* gpu    = static_cast<FrameUniforms*>(mapped.data);
    if (gpu != nullptr) {
        gpu->viewProj           = view.viewProjMatrix;
        gpu->unjitteredViewProj = unjittered;
        gpu->invViewProj        = unjittered.Inversed();
        gpu->invProj            = view.projMatrix.Inversed();
        std::memcpy(&gpu->camPos[0], &currentUniforms.camPos[0], sizeof(float) * 4);
    }
}

// ============================================================================
// Scene upload
// ============================================================================

void RenderContext::Impl::PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept {
    ApplySceneView(view);

    DispatchSkinningPasses(cmd);

    if (queues.drawQueue.size() > kGpuCullingMaxInstances) {
        queues.drawQueue.resize(kGpuCullingMaxInstances);
    }

    FlushLineQueue();
    SortDrawQueue();

    auto drawCount = queues.drawQueue.size();
    auto csgCount  = queues.csgDrawQueue.size();

    if (drawCount > 0 || csgCount > 0) {
        auto  mapped = frames.instanceDataBuffers[presenter.frameIndex].Map();
        auto* dst    = static_cast<InstanceData*>(mapped.data);

        for (size_t i = 0; i < drawCount; ++i) {
            dst[i] = queues.drawQueue[i].instanceData;
        }

        uint32_t csgOffset = drawCount;
        for (auto& csgCmd: queues.csgDrawQueue) {
            dst[csgOffset]        = csgCmd.eyeDraw.instanceData;
            csgCmd.eyeInstanceIdx = csgOffset++;

            for (auto& cutter: csgCmd.cutters) {
                dst[csgOffset]     = cutter.draw.instanceData;
                cutter.instanceIdx = csgOffset++;
            }
        }
    }
    BuildTLAS(cmd);
}

// ============================================================================
// Fork replayer
// ============================================================================
//
// Vk::Fork hands the sub-pass bodies here; the graph has already emitted every
// barrier the union of their usages needs. Threading is this layer's business,
// which is why the graph takes an executor instead of knowing about the task
// system.

namespace {

/// One forked sub-pass body, as a callable the recorder can hand a slot to.
struct ForkBodyCall {
    const Vk::ForkBody* body = nullptr;

    void operator()(Vk::RecordingSlot slot) const noexcept {
        (*body)(slot.cmd);
    }
};

/// Record the first N bodies into N secondaries. The count is the pack's, so
/// the recorder's static slot assertion is satisfied by construction.
template <size_t N, typename Recorder, typename Scheduler, size_t... Is>
void RecordForkBodies(Recorder& rec, Scheduler& scheduler, std::span<const Vk::ForkBody> bodies, std::index_sequence<Is...>) noexcept {
    const std::array<ForkBodyCall, N> calls {ForkBodyCall {&bodies[Is]}...};
    rec.Record(scheduler, calls[Is]...);
}

} // namespace

void RenderContext::Impl::ForkReplayer::ExecuteFork(VkCommandBuffer cmd, std::span<const Vk::ForkBody> bodies) noexcept {
    auto& self = *impl;

    using Recorder          = std::remove_reference_t<decltype(self.parallelRecorder[0])>;
    constexpr size_t kSlots = Recorder::Slots();
    const size_t     count  = bodies.size();

    // Measurement switch: the parallel path's fixed cost (heap rebind, a
    // scheduler round trip, two secondaries and an execute) is worth knowing on
    // a frame too small for it to win, and the only way to know it is to record
    // the same frame without it.
    if (Diag::ForkSequentialForced()) {
        for (const Vk::ForkBody& body: bodies) {
            body(cmd);
        }
        return;
    }

    // A single body is not worth a secondary, and more bodies than recorder
    // slots cannot be replayed: record them in stream order. Same barriers,
    // same resources -- just no threads.
    if (count < 2 || count > kSlots) {
        for (const Vk::ForkBody& body: bodies) {
            body(cmd);
        }
        return;
    }

    // The primary's heap state must be current before the secondaries inherit
    // it; the recorder re-pushes the per-frame address block into each of them
    // (push data is not inherited).
    self.BindHeapsAndPushFrame(cmd);

    auto& rec = self.parallelRecorder[0];
    rec.Reset();

    const auto samplerBind  = self.heapManager.GetSamplerHeapBindInfo();
    const auto resourceBind = self.heapManager.GetResourceHeapBindInfo();
    const auto frameAddrs   = self.FrameHeapAddresses();
    rec.SetHeapState(
        &samplerBind, &resourceBind, &self.ctx, Vk::kHeapPushDataLayout.frameAddressOffsets,
        std::span<const VkDeviceAddress> {frameAddrs.data(), frameAddrs.size()}
    );

    // Bodies must not rebind the heaps inside a secondary: doing so would
    // invalidate the primary's heap state after vkCmdExecuteCommands.
    const bool previousInheritance = self.forkSecondaries;
    self.forkSecondaries           = true;

    TaskSystemScheduler scheduler;
    // Dispatch on the runtime body count, but only into the arities this
    // recorder has slots for; anything wider was already recorded in stream.
    if (count == 2) {
        RecordForkBodies<2>(rec, scheduler, bodies, std::make_index_sequence<2> {});
    } else if constexpr (kSlots >= 3) {
        if (count == 3) {
            RecordForkBodies<3>(rec, scheduler, bodies, std::make_index_sequence<3> {});
        } else if constexpr (kSlots >= 4) {
            RecordForkBodies<4>(rec, scheduler, bodies, std::make_index_sequence<4> {});
        }
    }

    self.forkSecondaries = previousInheritance;

    Vk::ExecuteCommands(cmd, rec.GetCommandBuffers().first(bodies.size()));
}

// ============================================================================
// Frame lifecycle: synchronization, allocators and presentation only
// ============================================================================

auto RenderContext::BeginFrame() noexcept -> FrameOutcome<FrameSkipped> {
    // 1. Wait for the previous frame at this slot. Extra windows carry their own
    //    sync, waited one frame in flight exactly like the primary. The wait's
    //    own result is mapped like every other frame result (in practice it is
    //    a lost device -- the timeout is infinite -- and FrameResult::DeviceLost
    //    is what that is called here).
    if (const VkResult waited = _impl->presenter.sync.Wait(_impl->presenter.frameIndex ^ 1u); waited != VK_SUCCESS) {
        return std::unexpected(Vk::ToFrameError(waited));
    }
    for (auto& dest: _impl->destinations.Windows()) {
        if (dest.IsPrimary()) {
            continue;
        }
        auto& sess = dest.Presenter();
        if (const VkResult waited = sess.sync.Wait(sess.frameIndex ^ 1u); waited != VK_SUCCESS) {
            return std::unexpected(Vk::ToFrameError(waited));
        }
    }

    auto& stagingContext = _impl->stagingContext;
    auto& frame_index    = _impl->presenter.frameIndex;
    auto& deletionQueue  = _impl->deletionQueue;
    if (stagingContext) {
        stagingContext->Wait();
        stagingContext.reset();
    }

    deletionQueue.BeginFrame(frame_index);
    // Recycle texture slots whose release retired with this parity. Runs after
    // the fence wait and before the guard: the queue is idle, so the released
    // images die now rather than two frames from now.
    _impl->ReclaimTextureSlots(frame_index);
    _impl->activeQueueGuard.emplace(deletionQueue);
    // VK_EXT_descriptor_heap: rewind this frame's transient descriptor
    // partition, which every pass's block is allocated from.
    _impl->heapManager.BeginFrame(frame_index);
    // The UI vertex arena is per-frame for the same reason the descriptor
    // partition is: every RenderUI this frame appends to the slot the last one
    // used, so a second window's UI cannot land on the first window's vertices.
    _impl->uiRenderer.BeginFrame();

    // Retrieve GPU profiling results
    float timestampPeriod = _impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
    _impl->gpuProfiler.RetrieveResults(frame_index, timestampPeriod, [](std::string_view name, float durationMS) -> void {
        CPUProfiler::Record(name, durationMS);
    });

    // Pipeline counters for the frame that just completed (only while a
    // PipelineStatsCapture is live). Accumulated across frames until drained
    // via PipelineStatsCapture::Consume.
    _impl->gpuProfiler.RetrievePipelineStats(frame_index, [this](std::string_view, const Profiler::PipelineStats& stats) -> void {
        auto& acc = _impl->pendingPipelineCounters;
        acc.iaPrimitives += stats.iaPrimitives;
        acc.vsInvocations += stats.vsInvocations;
        acc.clipperInvocations += stats.clipperInvocations;
        acc.clipperPrimitivesOut += stats.clipperPrimitivesOut;
        acc.gsInvocations += stats.gsInvocations;
        acc.gsPrimitives += stats.gsPrimitives;
        acc.fsInvocations += stats.fsInvocations;
        acc.csInvocations += stats.csInvocations;
        acc.taskInvocations += stats.taskInvocations;
        acc.meshInvocations += stats.meshInvocations;
    });

    _impl->presenter.sync.StepTimeline(frame_index);

    // Reset query pools
    _impl->gpuProfiler.Reset(frame_index);
    _impl->computePools[frame_index].Reset();

    for (auto& worker: _impl->workerCmds) {
        worker.cmdCount[frame_index].store(0, std::memory_order::relaxed);
        worker.pools[frame_index].Reset();
    }

    // Per-frame scratch. The frame owns no command buffer: every destination
    // owns its own recording, and the frame's guard below ends whatever a
    // destination left open.
    _impl->destinations.BeginFrame();
    _impl->computeSubmittedThisFrame = false;
    _impl->hasSkinnedThisFrame       = false;
    _impl->sceneTarget.reset();

    auto& resized = _impl->resized;
    if (resized) {
        auto fbSize = GetFramebufferSize();
        if (!fbSize.has_value()) {
            // Nothing to draw into and nothing wrong: the window is minimised or
            // mid-resize. The frame is skipped, which is a value here and not an
            // error, so the caller's whole move is to carry on to the next frame.
            return FrameSkipped {};
        }

        VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

        if (!_impl->RecreateTargets(ext)) {
            return std::unexpected(FrameResult::TargetRecreationFailed);
        }

        // A recreated target's contents are undefined and its record is fresh.
        // The frame that records nothing into it does not present that
        // undefined image: EndFrame's FillUnwrittenDestinations fills it with
        // the scene background colour and says so in the log.
        resized = false;
    }

    return {};
}

auto RenderContext::EndFrame() noexcept -> FrameOutcome<PresentSuboptimal> {
    struct EndFrameGuard {
        RenderContext::Impl* impl;
        explicit EndFrameGuard(RenderContext::Impl* i) noexcept: impl(i) {
        }
        ~EndFrameGuard() noexcept {
            if (impl != nullptr) {
                // A frame never leaves a command buffer recording: whatever the
                // presentation path did not close (a present that failed, a
                // window that was never reached) is closed here.
                impl->destinations.CloseRecordings();
                impl->activeQueueGuard.reset();
                impl->queues.Clear();
                impl->hasSkinnedThisFrame       = false;
                impl->computeSubmittedThisFrame = false;
                impl->sceneTarget.reset();
                impl->destinations.SetActive(nullptr);
            }
        }
        EndFrameGuard(const EndFrameGuard&)                    = delete;
        auto operator=(const EndFrameGuard&) -> EndFrameGuard& = delete;
        EndFrameGuard(EndFrameGuard&&)                         = delete;
        auto operator=(EndFrameGuard&&) -> EndFrameGuard&      = delete;
    } frameGuard {_impl.get()};

    // Last chance to touch the frame's command buffers: a frame that vended a
    // destination but recorded nothing into it still has to hand presentation
    // defined contents, and presenting an image no pass wrote is what a black
    // frame under a green test suite looks like from the outside.
    _impl->FillUnwrittenDestinations();

    // Every window that was drawn into is closed, submitted and presented here.
    // A frame that vendored nothing still advances the schedule, so the
    // double-buffered state keeps alternating.
    const uint32_t primarySlotBefore = _impl->presenter.frameIndex;
    auto           presented         = _impl->PresentUsedWindows();
    if (_impl->presenter.frameIndex == primarySlotBefore) {
        _impl->presenter.frameIndex = (primarySlotBefore + 1) & 1u;
    }

    // The frame's uploads are submitted; the staging context can retire.
    if (_impl->stagingContext) {
        _impl->stagingContext->Wait();
        _impl->stagingContext.reset();
    }

    _impl->frames.FlipAll();

    std::swap(_impl->graphResources.shadowMap, _impl->shadowMapPrev);
    std::swap(_impl->shadowCascadeViews, _impl->shadowCascadeViewsPrev);
    std::swap(_impl->graphResources.voxelHistory, _impl->graphResources.voxelResolved);

    // Whatever the present calls said, already in the frame vocabulary: an
    // error (FrameResult::DeviceLost, or the driver's own code), or
    // PresentSuboptimal -- a frame that was drawn but not shown as asked, which
    // the renderer has already rebuilt for and which this returns as the value
    // it is.
    return presented;
}

// ============================================================================
// Opaque dispatches (the surface apps and the engine call)
// ============================================================================

auto RenderContext::AcquireTarget(const Window& window) noexcept -> FrameOutcome<RenderAttachment> {
    return _impl->AcquireTarget(window);
}

auto RenderContext::GetWindowAttachment(const Window& window) noexcept -> std::optional<RenderAttachment> {
    return _impl->WindowAttachment(window);
}

void RenderContext::ReleaseWindow(const Window& window) noexcept {
    _impl->ReleaseWindow(window);
}

auto RenderContext::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) -> std::expected<TextureHandle, ErrorCode> {
    return _impl->CreateRenderTexture(width, height, hdr);
}

void RenderContext::DestroyRenderTexture(TextureHandle handle) noexcept {
    _impl->DestroyRenderTexture(handle);
}

void RenderContext::RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept {
    // Resolve the destination once, by value: everything downstream (the blit
    // tail, the depth binding, the presentation booking) reads it from the
    // frame's scene target instead of assuming the primary swapchain.
    auto resolved = _impl->destinations.Resolve(view.target);
    if (!resolved) {
        // The miss carries the reason, so the handle is named once and the
        // question "retired, or re-vended to someone else?" is answered by the
        // registry instead of being re-derived from the raw handle here.
        const DestinationRegistry::Miss& miss = resolved.error();
        ZHLN::Log(
            "[RenderScene] Attachment 0x{:016X} (mip {}, layer {}) does not resolve to a live render target: {}.",
            static_cast<uint64_t>(view.target.texture), view.target.mipLevel, view.target.arrayLayer, miss.reason
        );

        // One miss is recoverable, and it is the one a frame-rebuild produces:
        // the caller holds the window attachment the *previous* generation
        // vended -- same slot, older serial -- and the frame has already
        // re-vended that slot. The registry says so (Miss::Adoptable) and hands
        // back the live record. Draw into it rather than presenting a frame
        // with nothing recorded into it. A miss for any other reason -- a
        // render texture that has been destroyed, a slot that went to another
        // destination -- stays a skip: drawing it into the window would be a
        // different lie.
        if (!miss.Adoptable()) {
            ZHLN::Log("[RenderScene] The view's target is not this frame's destination; scene skipped.");
            return;
        }
        ZHLN::Log(
            "[RenderScene] Adopting this frame's re-vended destination 0x{:016X} for that slot (serial {} -> {}).", miss.live->handle.Raw(),
            miss.asked.Serial(), miss.live->handle.Serial()
        );
        _impl->sceneTarget = *miss.live;
    } else {
        _impl->sceneTarget = *resolved;
    }
    _impl->settings = settings;

    // Which stream this pass records into is the target's answer, not the
    // context's: the destination the view names owns the command buffer it is
    // drawn with. A target that resolves but has no recording open is a
    // destination this frame never acquired -- recording it into whatever was
    // vended last is exactly what this call used to do.
    const VkCommandBuffer cmd = _impl->RecordingFor(*_impl->sceneTarget);
    if (cmd == VK_NULL_HANDLE) {
        ZHLN::Log(
            "[RenderScene] Destination 0x{:016X} has no recording open this frame (was it acquired?); scene skipped.", _impl->sceneTarget->handle.Raw()
        );
        return;
    }
    Pipelines::DeferredPbrPipeline::Execute(*_impl, cmd, view, settings);

    // The destination this frame vended has now been written. The facade owns
    // the bookkeeping that turns "vended" into "presentable", so it notes the
    // write here rather than leaving the pipeline to know about records; a
    // frame that recorded nothing is caught by FillUnwrittenDestinations.
    if (_impl->sceneTarget.has_value()) {
        _impl->destinations.NoteWritten(
            RenderAttachment {.texture = _impl->sceneTarget->handle.AsTexture(), .mipLevel = 0, .arrayLayer = 0}, Vk::AttachmentLayout::ColorAttachment
        );
    }
}

void RenderContext::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept {
    // No command buffer is passed here and none is read: the pass resolves the
    // view's target and records into that destination's stream, so a UI pass
    // cannot land in whichever window happened to be vended last.
    Pipelines::UIPipeline::Execute(*_impl, view, uiData);
}

void RenderContext::DispatchCompute(float dt) noexcept {
    Pipelines::ComputeSimPipeline::Submit(*_impl, dt);
}

void RenderContext::Impl::ProvokeDeviceLostInternal() const {
    if (!hangGpuPass.pipeline.Valid()) {
        return;
    }

    // hang_gpu.slang stores through 0x100 so the GPU MMU faults and the OS
    // TDR loses the device. CPU Vulkan (llvmpipe) would SIGSEGV a host
    // worker instead — skip the dispatch there.
    if (ctx.PhysicalInfo().properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        ZHLN::Log("[GPU] Skipping hang-GPU dispatch on CPU Vulkan device '{}'; it would SIGSEGV a worker thread.", ctx.PhysicalInfo().properties.properties.deviceName);
        return;
    }

    if (const VkCommandBuffer cmd = FrameCommand(); cmd != VK_NULL_HANDLE) {
        hangGpuPass.Bind(cmd);
        hangGpuPass.DispatchGroups(cmd, 1, 1, 1);
    } else {
        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto cmd) -> auto {
            hangGpuPass.Bind(cmd);
            hangGpuPass.DispatchGroups(cmd, 1, 1, 1);
        });
    }
}

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
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

auto IndirectTelemetryEnabled() noexcept -> bool {
    static const bool enabled = std::getenv("ZHLN_DEBUG_INDIRECT") != nullptr;
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
        ctx.BufferAddress(frames.frameUniformBuffers[session.frameIndex].Handle()), ctx.BufferAddress(frames.lightStorageBuffers[session.frameIndex].Handle()),
        ctx.BufferAddress(frames.instanceDataBuffers[session.frameIndex].Handle()), ctx.BufferAddress(frames.jointBuffers[session.frameIndex].Handle()),
        ctx.BufferAddress(frames.jointBuffers[session.frameIndex ^ 1].Handle()),    ctx.BufferAddress(morphDeltasBuffer.Handle()),
    };
}

void RenderContext::Impl::BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept {
    // Legacy descriptor-set and push-constant commands elsewhere in the frame
    // invalidate heap + push-data state (and vice versa), so every heap-based
    // segment re-binds both heaps and re-pushes the per-frame device addresses
    // that back the scene registry's PUSH_ADDRESS mappings.
    heapManager.BindHeaps(cmd);
    const auto addresses = FrameHeapAddresses();
    Vk::PushHeapFrameAddresses(ctx, cmd, heapPushDataLayout, addresses);
}

auto RenderContext::GetFramebufferSize() const -> std::optional<Extent2D> {
    Extent2D size = _impl->window.GetSize();
    if (size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    return size;
}

void RenderContext::Impl::DispatchSkinningPasses() {
    if (!hasSkinnedThisFrame) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Compute Skinning");
    auto* const       cmd = current_cmd;
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

            skinningPass.PushConstants(cmd, pcs);
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

    auto& instanceBuf = frames.tlasInstanceBuffers[session.frameIndex];

    // The instance buffer is host-visible and coherent (CPU_TO_GPU): write it
    // directly while recording. The memcpy completes before submission, and
    // the double-buffered parity means the previous TLAS build against this
    // buffer finished frames ago -- no staging copy, no transfer barrier.
    std::memcpy(instanceBuf.Map().data, tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    ZHLN_TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    rtCtx.BuildTLAS(cmd, geom, frames.tlas[session.frameIndex], ctx.BufferAddress(frames.tlasScratchBuffer[session.frameIndex].Handle()), tlasInstancesScratch.size());

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
    auto  mapped = frames.frameUniformBuffers[session.frameIndex].Map();
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
    current_cmd = cmd;
    ApplySceneView(view);

    DispatchSkinningPasses();

    if (queues.drawQueue.size() > kGpuCullingMaxInstances) {
        queues.drawQueue.resize(kGpuCullingMaxInstances);
    }

    FlushLineQueue();
    SortDrawQueue();

    auto drawCount = queues.drawQueue.size();
    auto csgCount  = queues.csgDrawQueue.size();

    if (drawCount > 0 || csgCount > 0) {
        auto  mapped = frames.instanceDataBuffers[session.frameIndex].Map();
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

    if (Diag::IndirectTelemetryEnabled()) {
        static uint32_t s_TelemetryFrame = 0;
        ++s_TelemetryFrame;
        if (s_TelemetryFrame >= 4 && (s_TelemetryFrame % 120) == 4) {
            DumpIndirectTelemetry(s_TelemetryFrame);
        }
    }
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
        &samplerBind, &resourceBind, &self.ctx, self.heapPushDataLayout.frameAddressOffsets,
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

auto RenderContext::BeginFrame() noexcept -> RenderResult {
    using enum RenderFrameResult;

    // 1. Wait for the previous frame at this slot. Extra windows carry their own
    //    sync, waited one frame in flight exactly like the primary.
    if (_impl->session.sync.Wait(_impl->session.frameIndex ^ 1u) == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(DeviceLost);
    }
    for (auto& dest: _impl->destinationWindows) {
        if (dest.IsPrimary()) {
            continue;
        }
        auto& sess = dest.Session();
        if (sess.sync.Wait(sess.frameIndex ^ 1u) == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(DeviceLost);
        }
    }

    auto& stagingContext = _impl->stagingContext;
    auto& frame_index    = _impl->session.frameIndex;
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

    _impl->session.sync.StepTimeline(frame_index);

    // Reset query pools
    _impl->gpuProfiler.Reset(frame_index);
    _impl->computePools[frame_index].Reset();

    for (auto& worker: _impl->workerCmds) {
        worker.cmdCount[frame_index].store(0, std::memory_order::relaxed);
        worker.pools[frame_index].Reset();
    }

    // Per-frame scratch: a frame owns the destination it vends and nothing else.
    _impl->current_cmd               = VK_NULL_HANDLE;
    _impl->current_image_index       = 0;
    _impl->activeDestinationWindow   = nullptr;
    _impl->computeSubmittedThisFrame = false;
    _impl->hasSkinnedThisFrame       = false;
    _impl->sceneTarget.reset();
    for (auto& dest: _impl->destinationWindows) {
        dest.imageAcquired = false;
        dest.openCmd       = VK_NULL_HANDLE;
        dest.commandOpen   = false;
    }

    auto& resized = _impl->resized;
    if (resized) {
        auto fbSize = GetFramebufferSize();
        if (!fbSize.has_value()) {
            return std::unexpected(OutOfDate);
        }

        VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

        if (!_impl->RecreateTargets(ext)) {
            return std::unexpected(Error);
        }

        // A recreated target's contents are undefined and its record is fresh.
        // The frame that records nothing into it does not present that
        // undefined image: EndFrame's FillUnwrittenDestinations fills it with
        // the scene background colour and says so in the log.
        resized = false;
    }

    return {};
}

auto RenderContext::EndFrame() noexcept -> RenderResult {
    struct EndFrameGuard {
        RenderContext::Impl* impl;
        explicit EndFrameGuard(RenderContext::Impl* i) noexcept: impl(i) {
        }
        ~EndFrameGuard() noexcept {
            if (impl != nullptr) {
                impl->activeQueueGuard.reset();
                impl->queues.Clear();
                impl->current_cmd               = VK_NULL_HANDLE;
                impl->hasSkinnedThisFrame       = false;
                impl->computeSubmittedThisFrame = false;
                impl->sceneTarget.reset();
                impl->activeDestinationWindow = nullptr;
            }
        }
        EndFrameGuard(const EndFrameGuard&)                    = delete;
        auto operator=(const EndFrameGuard&) -> EndFrameGuard& = delete;
        EndFrameGuard(EndFrameGuard&&)                         = delete;
        auto operator=(EndFrameGuard&&) -> EndFrameGuard&      = delete;
    } frameGuard {_impl.get()};

    using enum RenderFrameResult;

    // Last chance to touch the frame's command buffers: a frame that vended a
    // destination but recorded nothing into it still has to hand presentation
    // defined contents, and presenting an image no pass wrote is what a black
    // frame under a green test suite looks like from the outside.
    _impl->FillUnwrittenDestinations();

    // Every window that was drawn into is closed, submitted and presented here.
    // A frame that vendored nothing still advances the schedule, so the
    // double-buffered state keeps alternating.
    const uint32_t primarySlotBefore = _impl->session.frameIndex;
    auto           presented         = _impl->PresentUsedWindows();
    if (_impl->session.frameIndex == primarySlotBefore) {
        _impl->session.frameIndex = (primarySlotBefore + 1) & 1u;
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

    if (!presented.has_value()) {
        if (presented.error() == ErrorCode {Suboptimal}) {
            return std::unexpected(Suboptimal);
        }
        return std::unexpected(presented.error());
    }
    return {};
}

// ============================================================================
// Opaque dispatches (the surface apps and the engine call)
// ============================================================================

auto RenderContext::GetWindowAttachment(const Window& window) noexcept -> RenderAttachment {
    return _impl->VendedWindowAttachment(window);
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
    if (_impl->current_cmd == VK_NULL_HANDLE) {
        ZHLN::Log("[RenderScene] No frame is open, or no destination was vended; scene skipped.");
        return;
    }
    // Resolve the destination once, by value: everything downstream (the blit
    // tail, the depth binding, the presentation booking) reads it from the
    // frame's scene target instead of assuming the primary swapchain.
    _impl->sceneTarget = _impl->ResolveAttachment(view.target);
    if (!_impl->sceneTarget.has_value()) {
        // Name the handle: a resolve miss means the view points at a record
        // that has been retired or recycled, and which handle it is tells a
        // reader whether the caller vendored the attachment this frame.
        ZHLN::Log(
            "[RenderScene] Attachment 0x{:016X} (mip {}, layer {}) does not resolve to a live render target.", static_cast<uint64_t>(view.target.texture),
            view.target.mipLevel, view.target.arrayLayer
        );

        // One case is recoverable, and it is the one a frame-rebuild produces:
        // the caller holds the window attachment the *previous* generation
        // vended -- same slot, older serial -- and the frame has already
        // vended the live record for that same slot. Draw into the live one
        // rather than presenting a frame with nothing recorded into it. A
        // handle for a genuinely different record (a render texture that has
        // been destroyed, say) stays a skip: drawing it into the window would
        // be a different lie.
        const auto stale    = Impl::DecodeRenderHandle(static_cast<uint64_t>(view.target.texture));
        auto       live     = _impl->ActiveDestinationRecord();
        bool       adopted  = false;
        if (stale.has_value() && live.has_value()) {
            const auto liveHandle = Impl::DecodeRenderHandle(static_cast<uint64_t>(live->handle));
            if (liveHandle.has_value() && liveHandle->index == stale->index) {
                ZHLN::Log(
                    "[RenderScene] Adopting this frame's re-vended destination 0x{:016X} for that slot (serial {} -> {}).",
                    static_cast<uint64_t>(live->handle), stale->serial, liveHandle->serial
                );
                _impl->sceneTarget = std::move(live);
                adopted            = true;
            }
        }
        if (!adopted) {
            ZHLN::Log("[RenderScene] The view's target is not this frame's destination; scene skipped.");
            return;
        }
    }
    _impl->settings = settings;
    Pipelines::DeferredPbrPipeline::Execute(*_impl, _impl->current_cmd, view, settings);

    // The destination this frame vended has now been written. The facade owns
    // the bookkeeping that turns "vended" into "presentable", so it notes the
    // write here rather than leaving the pipeline to know about records; a
    // frame that recorded nothing is caught by FillUnwrittenDestinations.
    if (_impl->sceneTarget.has_value()) {
        _impl->NoteAttachmentWritten(
            RenderAttachment {.texture = _impl->sceneTarget->handle, .mipLevel = 0, .arrayLayer = 0}, Vk::AttachmentLayout::ColorAttachment
        );
    }
}

void RenderContext::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept {
    if (_impl->current_cmd == VK_NULL_HANDLE) {
        return;
    }
    Pipelines::UIPipeline::Execute(*_impl, _impl->current_cmd, view, uiData);
}

void RenderContext::DispatchCompute(float dt) noexcept {
    Pipelines::ComputeSimPipeline::Submit(*_impl, dt);
}

void RenderContext::Impl::RecordIndirectTelemetry(VkCommandBuffer cmd) noexcept {
    if (!indirectReadbackReady) {
        bool ok = true;
        for (uint32_t i = 0; i < 2; ++i) {
            auto rb = Vk::Buffer::Create(allocator.Get(), kTelemetryReadbackBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
            if (!rb) {
                ok = false;
                break;
            }
            indirectReadbackBuffers[i] = std::move(*rb);
        }
        if (!ok) {
            ZHLN::Log("[Diag] Failed to allocate indirect telemetry readback buffers; telemetry disabled.");
            return;
        }
        indirectReadbackReady = true;
    }

    const auto bytes = sizeof(VkDrawIndirectCommand) * kTelemetryMaxDraws;

    // Make the culling writes visible to the transfer stage before copying.
    Vk::BufferBarrier(
        cmd, frames.indirectCommandsBuffers[session.frameIndex], Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::TransferRead
    );
    Vk::BufferBarrier(
        cmd, frames.indirectCommandsBuffersPass2[session.frameIndex], Vk::BarrierStage::Compute | Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferRead
    );
    Vk::BufferBarrier(
        cmd, frames.secondPassCountBuffers[session.frameIndex], Vk::BarrierStage::Compute | Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferRead
    );

    auto& dst = indirectReadbackBuffers[session.frameIndex];
    Vk::CopyBuffer(cmd, frames.indirectCommandsBuffers[session.frameIndex], dst, bytes, 0, kTelemetryPass1Offset);
    Vk::CopyBuffer(cmd, frames.indirectCommandsBuffersPass2[session.frameIndex], dst, bytes, 0, kTelemetryPass2Offset);
    Vk::CopyBuffer(cmd, frames.secondPassCountBuffers[session.frameIndex], dst, sizeof(uint32_t), 0, kTelemetryCountOffset);
}

void RenderContext::Impl::DumpIndirectTelemetry(uint32_t frameNo) noexcept {
    const auto drawCount = static_cast<uint32_t>(queues.drawQueue.size());

    const bool useGpuCulling = cullingPass.pipeline.Valid() && frames.indirectCommandsBuffers->Valid() && (drawCount <= kGpuCullingMaxInstances) &&
                               !Diag::DisableGpuCulling() && !MeshShadingActive();

    ZHLN::Log("[Diag] ---- frame {}: draws={} gpuCulling={} meshShading={} ----", frameNo, drawCount, useGpuCulling ? 1 : 0, MeshShadingActive() ? 1 : 0);

    if (drawCount == 0) {
        return;
    }

    // Mirror of MainPass1/2's group building so the log shows the exact
    // indirect ranges the walker consumes this frame.
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    uint32_t   groupStart      = 0;
    for (uint32_t i = 0; i < drawCount; ++i) {
        const auto&       drawCmd     = queues.drawQueue[i];
        const auto* const drawMat     = drawCmd.material;
        const bool        forwardOnly = (drawCmd.instanceData.flags & 0xFF) == 2;
        const bool        viewmodel   = (drawCmd.flags & DrawFlags::Viewmodel) != DrawFlags::None;
        const bool        matValid    = (drawMat != nullptr) && drawMat->pipeline.Valid();

        if (forwardOnly || viewmodel || !matValid) {
            ZHLN::Log("[Diag]   draw {} EXCLUDED from main passes (fwdOnly={} vm={} matValid={})", i, forwardOnly ? 1 : 0, viewmodel ? 1 : 0, matValid ? 1 : 0);
            currentPipeline = VK_NULL_HANDLE;
            continue;
        }

        if (i == 0 || drawMat->pipeline.Get() != currentPipeline) {
            groupStart      = i;
            currentPipeline = drawMat->pipeline.Get();
            ZHLN::Log("[Diag]   group @{} pipeline=0x{:x}", groupStart, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(currentPipeline)));
        } else {
            ZHLN::Log("[Diag]   draw {} extends group @{}", i, groupStart);
        }
    }

    const uint32_t n = std::min(drawCount, kTelemetryMaxDraws);
    for (uint32_t i = 0; i < n; ++i) {
        const auto& dq = queues.drawQueue[i].instanceData;
        const auto  tr = dq.world.GetTranslation();
        ZHLN::Log(
            "[Diag]   queue inst[{}]: vtx={} idx={} posAddr=0x{:x} iboAddr=0x{:x} radius={:.3f} t=({:.2f},{:.2f},{:.2f})", i, dq.vertexCount, dq.indexCount,
            static_cast<uint64_t>(dq.posAddress), static_cast<uint64_t>(dq.iboAddress), dq.cullRadius, tr.GetX(), tr.GetY(), tr.GetZ()
        );
    }

    if (indirectReadbackReady) {
        auto        mapped = indirectReadbackBuffers[session.frameIndex].Map();
        const auto* bytes  = static_cast<const uint8_t*>(mapped.data);
        if (bytes != nullptr) {
            const auto* pass1 = reinterpret_cast<const VkDrawIndirectCommand*>(bytes + kTelemetryPass1Offset);
            const auto* pass2 = reinterpret_cast<const VkDrawIndirectCommand*>(bytes + kTelemetryPass2Offset);
            const auto* count = reinterpret_cast<const uint32_t*>(bytes + kTelemetryCountOffset);
            ZHLN::Log("[Diag]   retired (frame-2) pass2 candidateCount={}", *count);
            for (uint32_t i = 0; i < n; ++i) {
                ZHLN::Log(
                    "[Diag]   cmd[{}] pass1(vc={} ic={} fv={} fi={}) pass2(vc={} ic={} fv={} fi={})", i, pass1[i].vertexCount, pass1[i].instanceCount,
                    pass1[i].firstVertex, pass1[i].firstInstance, pass2[i].vertexCount, pass2[i].instanceCount, pass2[i].firstVertex, pass2[i].firstInstance
                );
            }
        }
    }
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

    if (current_cmd != VK_NULL_HANDLE) {
        hangGpuPass.Bind(current_cmd);
        hangGpuPass.DispatchGroups(current_cmd, 1, 1, 1);
    } else {
        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto cmd) -> auto {
            hangGpuPass.Bind(cmd);
            hangGpuPass.DispatchGroups(cmd, 1, 1, 1);
        });
    }
}

} // namespace ZHLN

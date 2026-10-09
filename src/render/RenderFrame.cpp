// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "graph/RenderGraph.hpp"

#include <ShaderBindings.hpp>

#include "pipelines/ComputeSimPipeline.hpp"
#include "pipelines/DeferredPbrPipeline.hpp"
#include "pipelines/UIPipeline.hpp"
#include "Zahlen/Profiler.hpp"
#include <Zahlen/Log.hpp>
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

}

namespace {

template <typename... Ptrs>
[[nodiscard]] constexpr auto AnyNull(Ptrs... ptrs) noexcept -> bool {
    return (... || (ptrs == nullptr));
}

}


auto RenderContext::Impl::FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount> {
    return {
        ctx.BufferAddress(frames.frameUniformBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.lightStorageBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.instanceDataBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex].Handle()),
        ctx.BufferAddress(frames.jointBuffers[Vk::PreviousFrameSlot(presenter.frameIndex)].Handle()),
        ctx.BufferAddress(morphDeltasBuffer.Handle()),
    };
}

void RenderContext::Impl::BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept {
    heapManager.BindHeaps(cmd);
    const auto addresses = FrameHeapAddresses();
    Vk::PushHeapFrameAddresses(cmd, GpuAbi::kScenePushLayout, addresses);
}

auto RenderContext::GetFramebufferSize() const -> std::optional<Extent2D> {
    Extent2D size = _impl->presentationTarget.GetFramebufferExtent();
    if (size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    return size;
}

void RenderContext::Impl::DispatchSkinningPasses(VkCommandBuffer cmd) {
    if (!frameState.hasSkinned || cmd == VK_NULL_HANDLE) {
        return;
    }

    ZHLN::ScopedTimer profTimer("GPU Compute Skinning");
    skinningPass.Bind(cmd);

    for (const auto& drawCmd: queues.Draws()) {
        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            auto* posMesh     = drawCmd.posMesh;
            auto* frameMesh   = drawCmd.frameMesh;
            auto* skinMesh    = drawCmd.skinMesh;
            auto* scratchMesh = geometry.Resolve(drawCmd.skinnedVertexBuffer);

            if (AnyNull(posMesh, frameMesh, scratchMesh) || posMesh->vertexCount > scratchMesh->vertexCount) {
                continue;
            }

            // Only position and tangent-frame streams are animated; the
            // immutable surface stream is read directly by the draw shader.
            const Vk::BufferSlice output {scratchMesh->buffer, scratchMesh->vboAddress};
            const VkDeviceSize posBytes   = static_cast<VkDeviceSize>(scratchMesh->vertexCount) * sizeof(VertexPosition);
            const VkDeviceSize frameBytes = static_cast<VkDeviceSize>(scratchMesh->vertexCount) * sizeof(VertexTangentFrame);
            const auto         positions  = output.Subspan(0, posBytes);
            const auto         framesOut  = output.Subspan(posBytes, frameBytes);
            if (positions.Size() != posBytes || framesOut.Size() != frameBytes) {
                continue;
            }

            SkinningConstants pcs {
                .inPosAddr        = posMesh->vboAddress,
                .inFrameAddr      = frameMesh->vboAddress,
                .inSkinAddr       = (skinMesh != nullptr) ? skinMesh->vboAddress : 0,
                .outPosAddr       = positions.Address(),
                .outFrameAddr     = framesOut.Address(),
                .jointsAddr       = ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex].Handle()),
                .morphDeltasAddr  = ctx.BufferAddress(morphDeltasBuffer.Handle()),
                .vertexCount      = posMesh->vertexCount,
                .jointOffset      = drawCmd.jointOffset,
                .morphOffset      = drawCmd.morphOffset,
                .activeMorphCount = drawCmd.activeMorphCount,
                .morphWeights     = drawCmd.morphWeights
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

    if (ctx.RayTracingSupported()) {
        ZHLN::ScopedTimer profTimerBLAS("GPU Skinned BLAS Rebuilds");
        for (const auto& drawCmd: queues.Draws()) {
            if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
                auto* scratchMesh = geometry.Resolve(drawCmd.skinnedVertexBuffer);
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
    if (!ctx.RayTracingSupported() || queues.Draws().empty()) {
        return;
    }

    tlasInstancesScratch.clear();
    tlasInstancesScratch.reserve(queues.Draws().size());

    using enum DrawFlags;

    for (uint32_t i = 0; i < queues.Draws().size(); ++i) {
        const auto& drawCmd = queues.Draws()[i];
        auto*       mesh    = drawCmd.posMesh;

        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            mesh = geometry.Resolve(drawCmd.skinnedVertexBuffer);
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

    auto mappedInstances = instanceBuf.Map(allocator);
    if (!mappedInstances) return;
    std::memcpy(mappedInstances->Data(), tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    Vk::TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    auto& scratch = frames.tlasScratchBuffer[presenter.frameIndex];
    Vk::BuildTLAS(cmd, geom, frames.tlas[presenter.frameIndex].Get(), Vk::BufferSlice {scratch, ctx.BufferAddress(scratch.Handle())},
                  tlasInstancesScratch.size());

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite,
        Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead
    );
}


void RenderContext::Impl::ApplySceneView(const SceneView& view) noexcept {
    const JPH::Mat44 unjittered = view.projMatrix * view.viewMatrix;

    current_view_proj    = view.viewProjMatrix;
    unjittered_view_proj = unjittered;

    currentUniforms.viewProj           = view.viewProjMatrix;
    currentUniforms.unjitteredViewProj = unjittered;
    currentUniforms.invViewProj        = unjittered.Inversed();
    currentUniforms.camPos = JPH::Float4 {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time};

    auto mapped = frames.frameUniformBuffers[presenter.frameIndex].Map(allocator);
    if (!mapped) {
        return;
    }
    auto* gpu               = mapped->As<FrameUniforms>();
    gpu->viewProj           = view.viewProjMatrix;
    gpu->unjitteredViewProj = unjittered;
    gpu->invViewProj        = unjittered.Inversed();
    gpu->invProj            = view.projMatrix.Inversed();
    gpu->camPos             = currentUniforms.camPos;
}


void RenderContext::Impl::PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept {
    ApplySceneView(view);

    DispatchSkinningPasses(cmd);

    if (queues.Draws().size() > kGpuCullingMaxInstances) {
        queues.Draws().resize(kGpuCullingMaxInstances);
    }

    FlushLineQueue();
    queues.Sort();

    auto drawCount = queues.Draws().size();
    auto csgCount  = queues.CsgDraws().size();

    if (drawCount > 0 || csgCount > 0) {
        auto mapped = frames.instanceDataBuffers[presenter.frameIndex].Map(allocator);
        if (!mapped) {
            activeLineVertexCount = 0;
            queues.Draws().clear();
            queues.CsgDraws().clear();
            return;
        }
        auto* dst = mapped->As<InstanceData>();

        for (size_t i = 0; i < drawCount; ++i) {
            dst[i] = queues.Draws()[i].instanceData;
        }

        uint32_t csgOffset = drawCount;
        for (auto& csgCmd: queues.CsgDraws()) {
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


namespace {

template <typename Recorder, typename Scheduler, size_t... Is>
auto RecordForkBodies(Recorder& rec, Scheduler& scheduler, std::span<const Vk::ForkCall> bodies, std::index_sequence<Is...>) noexcept
    -> std::expected<void, ErrorCode> {
    // Record() waits for its workers before returning. These references cannot
    // outlive the graph's pass objects or the local array of callable views.
    return rec.Record(scheduler, ([&body = bodies[Is]](Vk::RecordingSlot slot) noexcept { body(slot.cmd); })...);
}

}

void RenderContext::Impl::ForkReplayer::ExecuteFork(VkCommandBuffer cmd, std::span<const Vk::ForkCall> bodies) noexcept {
    auto& self = *impl;

    using Recorder          = std::remove_reference_t<decltype(self.parallelRecorders[0])>;
    constexpr size_t kSlots = Recorder::Slots();
    const size_t     count  = bodies.size();

    if (Diag::ForkSequentialForced()) {
        for (const Vk::ForkCall& body: bodies) {
            body(cmd);
        }
        return;
    }

    if (count < 2 || count > kSlots) {
        for (const Vk::ForkCall& body: bodies) {
            body(cmd);
        }
        return;
    }

    self.BindHeapsAndPushFrame(cmd);

    auto& rec = self.parallelRecorders[self.presenter.frameIndex];
    rec.Reset();

    const auto samplerBind  = self.heapManager.GetSamplerHeapBindInfo();
    const auto resourceBind = self.heapManager.GetResourceHeapBindInfo();
    const auto frameAddrs   = self.FrameHeapAddresses();
    rec.SetHeapState(
        &samplerBind, &resourceBind, GpuAbi::kScenePushLayout.UsedFrameAddresses(),
        std::span<const VkDeviceAddress> {frameAddrs.data(), frameAddrs.size()}
    );

    const bool previousInheritance  = self.frameState.inForkSecondary;
    self.frameState.inForkSecondary = true;

    TaskSystemScheduler scheduler;
    std::expected<void, ErrorCode> recorded;
    if (count == 2) {
        recorded = RecordForkBodies(rec, scheduler, bodies, std::make_index_sequence<2> {});
    } else if constexpr (kSlots >= 3) {
        if (count == 3) {
            recorded = RecordForkBodies(rec, scheduler, bodies, std::make_index_sequence<3> {});
        } else if constexpr (kSlots >= 4) {
            recorded = RecordForkBodies(rec, scheduler, bodies, std::make_index_sequence<4> {});
        }
    }

    self.frameState.inForkSecondary = previousInheritance;
    if (!recorded) {
        ZHLN::Log("[Render] Secondary command recording failed ({}); skipping this fork.", recorded.error());
        return;
    }
    Vk::ExecuteCommands(cmd, rec.GetCommandBuffers().first(bodies.size()));
}


auto RenderContext::BeginFrame() noexcept -> FrameOutcome<FrameSkipped> {
    // The two-state accumulation, shadow-map and voxel histories can be reused
    // only after the preceding GPU frame finishes. N-slot CPU/GPU buffers are
    // independently indexed, but increasing N alone does not permit additional
    // overlapping GPU frames while these history targets remain shared.
    auto& primarySync = _impl->presenter.sync;
    const uint32_t slot = _impl->presenter.frameIndex;
    // The previous frame protects shared two-state histories. The current
    // slot's older submission must also finish before its pools are reset, even
    // if the previous frame acquired no image and never submitted graphics.
    for (const uint32_t frameSlot: {Vk::PreviousFrameSlot(slot), slot}) {
        if (const VkResult waited = primarySync.Wait(frameSlot); waited != VK_SUCCESS) {
            return std::unexpected(Vk::ToError(waited));
        }
        // Compute can be submitted without a graphics frame (minimized or
        // skipped acquisition), so its timeline must be waited independently.
        if (const VkResult waited = primarySync.WaitCompute(frameSlot); waited != VK_SUCCESS) {
            return std::unexpected(Vk::ToError(waited));
        }
    }
    for (auto& dest: _impl->destinations.Windows()) {
        if (dest.IsPrimary()) {
            continue;
        }
        auto& sess = dest.Presenter();
        for (const uint32_t frameSlot: {Vk::PreviousFrameSlot(sess.frameIndex), sess.frameIndex}) {
            if (const VkResult waited = sess.sync.Wait(frameSlot); waited != VK_SUCCESS) {
                return std::unexpected(Vk::ToError(waited));
            }
        }
    }

    auto& submittedStaging = _impl->submittedStaging;
    auto& frame_index      = _impl->presenter.frameIndex;
    auto& deletionQueue    = _impl->deletionQueue;
    if (submittedStaging) {
        submittedStaging->Wait();
        submittedStaging.reset();
    }

    deletionQueue.BeginFrame(frame_index);
    _impl->textureManager.BeginFrame(frame_index);
    _impl->frameOpen = true;
    _impl->heapManager.BeginFrame(frame_index);
    _impl->uiRenderer.BeginFrame();

    float timestampPeriod = _impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
    _impl->gpuProfiler.RetrieveResults(frame_index, timestampPeriod, [](std::string_view name, float durationMS) -> void {
        CPUProfiler::Record(name, durationMS);
    });

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

    (void)_impl->presenter.sync.StepTimeline(frame_index);

    _impl->gpuProfiler.Reset(frame_index);
    _impl->computePools[frame_index].Reset();

    for (auto& worker: _impl->workerCmds) {
        worker.cmdCount[frame_index].store(0, std::memory_order::relaxed);
        worker.pools[frame_index].Reset();
    }

    _impl->destinations.BeginFrame();
    ++_impl->frameSerial;
    _impl->EvictInactiveEmitters();
    // This frame's billboard batches start again from their first slot. The serial
    // tells same-key submissions apart within one frame, so it is frame state and is
    // reset here, with frameSerial -- the emitter queue it counts into is shared with
    // the particle submits and cleared at EndFrame, so it cannot signal a frame.
    _impl->billboardSerial = 0;
    _impl->frameState.Reset();
    _impl->sceneTarget.reset();

    auto& resized = _impl->frameState.resized;
    if (resized) {
        auto fbSize = GetFramebufferSize();
        if (!fbSize.has_value()) {
            return FrameSkipped {};
        }

        VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

        if (!_impl->RecreateTargets(ext)) {
            return std::unexpected(FrameResult::TargetRecreationFailed);
        }

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
                impl->destinations.AbortRecordings();
                impl->frameOpen = false;
                impl->queues.Clear();
                impl->frameState.Reset();
                impl->sceneTarget.reset();
                impl->destinations.SetActive(0);
            }
        }
        EndFrameGuard(const EndFrameGuard&)                    = delete;
        auto operator=(const EndFrameGuard&) -> EndFrameGuard& = delete;
        EndFrameGuard(EndFrameGuard&&)                         = delete;
        auto operator=(EndFrameGuard&&) -> EndFrameGuard&      = delete;
    } frameGuard {_impl.get()};

    const uint32_t primarySlotBefore = _impl->presenter.frameIndex;
    auto           presented         = _impl->PresentUsedWindows();
    if (_impl->presenter.frameIndex == primarySlotBefore) {
        _impl->presenter.AdvanceFrame();
    }

    if (_impl->submittedStaging) {
        _impl->submittedStaging->Wait();
        _impl->submittedStaging.reset();
    }

    _impl->accumulationHistory.Swap();

    // These histories remain named graph resources, so exchange their roles
    // explicitly. BeginFrame's preceding-frame waits protect their reuse.
    std::swap(_impl->graphResources.shadowMap, _impl->targets.ShadowMapPrev());
    std::swap(_impl->targets.CascadeViews(), _impl->targets.CascadeViewsPrev());
    std::swap(_impl->graphResources.voxelHistory, _impl->graphResources.voxelResolved);

    return presented;
}


auto RenderContext::AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<FrameTarget> {
    return _impl->AcquireTarget(target);
}

auto RenderContext::GetAcquiredTarget(const PresentationTarget& target) noexcept -> std::optional<FrameTarget> {
    return _impl->TargetAttachment(target);
}

void RenderContext::ReleaseTarget(const PresentationTarget& target) noexcept {
    _impl->ReleaseTarget(target);
}

auto RenderContext::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) -> std::expected<RenderTextureHandle, ErrorCode> {
    return _impl->CreateRenderTexture(width, height, hdr);
}

void RenderContext::DestroyRenderTexture(RenderTextureHandle handle) noexcept {
    _impl->DestroyRenderTexture(handle);
}

auto RenderContext::RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept -> FrameOutcome<FrameSkipped> {
    auto resolved = _impl->ResolveTarget(view.target);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    const VkCommandBuffer cmd = resolved->recorder.Handle();
    _impl->destinations.SetActive(resolved->window.id);
    _impl->sceneTarget = resolved->image;
    _impl->settings = settings;

    Pipelines::DeferredPbrPipeline::Execute(*_impl, cmd, view, settings);

    _impl->sceneTarget.reset();
    resolved->layout = Vk::AttachmentLayout::ColorAttachment;
    resolved->drawn = true;
    _impl->warnedUnwrittenTarget = false;
    return std::nullopt;
}

auto RenderContext::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped> {
    return Pipelines::UIPipeline::Execute(*_impl, view, uiData);
}

auto RenderContext::DispatchSimulations(float dt) noexcept -> RenderResult {
    return Pipelines::ComputeSimPipeline::Submit(*_impl, dt);
}

void RenderContext::Impl::ProvokeDeviceLostInternal() const {
    if (!hangGpuPass.pipeline.Valid()) {
        return;
    }

    if (ctx.PhysicalInfo().properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        ZHLN::Log("[GPU] Skipping hang-GPU dispatch on CPU Vulkan device '{}'; it would SIGSEGV a worker thread.", ctx.PhysicalInfo().properties.properties.deviceName);
        return;
    }

    if (const VkCommandBuffer cmd = FrameCommand(); cmd != VK_NULL_HANDLE) {
        hangGpuPass.Bind(cmd);
        hangGpuPass.DispatchGroups(cmd, 1, 1, 1);
    } else {
        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto immediateCmd) -> auto {
            hangGpuPass.Bind(immediateCmd);
            hangGpuPass.DispatchGroups(immediateCmd, 1, 1, 1);
        });
    }
}

}

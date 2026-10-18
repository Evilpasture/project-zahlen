// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "graph/RenderGraph.hpp"
#include "pipelines/ComputeSimPipeline.hpp"
#include "pipelines/DeferredPbrPipeline.hpp"
#include "pipelines/UIPipeline.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/SceneData.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>
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

auto RenderFrameContext::FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount> {
    std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount> addresses {};
    for (const auto& entry : Slang::kFrameAddressMeta) {
        // Engine policy: which CPU resource serves each named field. Field
        // placement (offset, binding, order) is generated metadata; this
        // choice is not.
        addresses[static_cast<size_t>(entry.field)] = [&]() noexcept -> VkDeviceAddress {
            switch (entry.field) {
                case Slang::FrameAddressField::frameAddress:
                    return ctx.BufferAddress(frames.frameUniformBuffers[presenter.frameIndex].Handle());
                case Slang::FrameAddressField::lightsAddress:
                    return ctx.BufferAddress(frames.lightStorageBuffers[presenter.frameIndex].Handle());
                case Slang::FrameAddressField::instancesAddress:
                    return ctx.BufferAddress(frames.instanceDataBuffers[presenter.frameIndex].Handle());
                case Slang::FrameAddressField::jointsAddress:
                    return ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex].Handle());
                case Slang::FrameAddressField::previousJointsAddress:
                    return ctx.BufferAddress(frames.jointBuffers[Vk::PreviousFrameSlot(presenter.frameIndex)].Handle());
                case Slang::FrameAddressField::morphDeltasAddress:
                    return ctx.BufferAddress(morphDeltasBuffer.Handle());
            }
        }();
    }
    return addresses;
}

void RenderFrameContext::BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept {
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

auto RenderContext::Impl::DispatchSkinningPasses(VkCommandBuffer cmd) -> SkinnedBlasOutcome {
    SkinnedBlasOutcome outcome;
    if (!frameState.hasSkinned || cmd == VK_NULL_HANDLE) {
        return outcome;
    }

    ZHLN::ScopedTimer profTimer("GPU Compute Skinning");

    for (const auto& drawCmd: queues.Draws()) {
        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            auto* scratchMesh = geometry.Resolve(drawCmd.skinnedVertexBuffer);

            const uint32_t vertexCount = drawCmd.instanceData.vertexCount;
            if (drawCmd.skinPosAddress == nullptr || drawCmd.skinFrameAddress == nullptr || scratchMesh == nullptr || vertexCount > scratchMesh->vertexCount) {
                continue;
            }

            const Vk::BufferSlice output {scratchMesh->buffer, scratchMesh->vboAddress.Get()};
            const VkDeviceSize    posBytes   = static_cast<VkDeviceSize>(scratchMesh->vertexCount) * sizeof(VertexPosition);
            const VkDeviceSize    frameBytes = static_cast<VkDeviceSize>(scratchMesh->vertexCount) * sizeof(VertexTangentFrame);
            const auto            positions  = output.Subspan(0, posBytes);
            const auto            framesOut  = output.Subspan(posBytes, frameBytes);
            if (positions.Size() != posBytes || framesOut.Size() != frameBytes) {
                continue;
            }

            SkinningConstants pcs {
                .inPosAddr        = drawCmd.skinPosAddress.Get(),
                .inFrameAddr      = drawCmd.skinFrameAddress.Get(),
                .inSkinAddr       = drawCmd.skinDataAddress.Get(),
                .outPosAddr       = positions.Address().Get(),
                .outFrameAddr     = framesOut.Address().Get(),
                .jointsAddr       = ctx.BufferAddress(frames.jointBuffers[presenter.frameIndex].Handle()),
                .morphDeltasAddr  = ctx.BufferAddress(morphDeltasBuffer.Handle()),
                .vertexCount      = vertexCount,
                .jointOffset      = drawCmd.jointOffset,
                .morphOffset      = drawCmd.morphOffset,
                .activeMorphCount = drawCmd.activeMorphCount,
                .morphWeights     = drawCmd.morphWeights
            };

            compute.skinning.DispatchHeapThreads<Shaders::Modules::SkinningCS>(cmd, vertexCount, 1, 1, pcs);
        }
    }

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::AccelerationStructureBuild | Vk::BarrierStage::Vertex,
        Vk::BarrierAccess::AccelerationStructureRead | Vk::BarrierAccess::ShaderRead
    );

    if (ctx.RayTracingSupported()) {
        ZHLN::ScopedTimer profTimerBLAS("GPU Skinned BLAS Rebuilds");
        for (const auto& drawCmd: queues.Draws()) {
            if (drawCmd.skinnedVertexBuffer == BufferHandle::Invalid) {
                continue;
            }

            const bool alreadyRebuilt =
                std::ranges::any_of(outcome.addresses, [&](const SkinnedBlasEntry& entry) { return entry.scratch == drawCmd.skinnedVertexBuffer; });
            if (alreadyRebuilt) {
                continue;
            }
            auto* scratchMesh = geometry.Resolve(drawCmd.skinnedVertexBuffer);
            if (scratchMesh == nullptr) {
                continue;
            }
            const auto rebuilt = RebuildSkinnedBLAS(cmd, ctx, allocator, deletionQueue, drawCmd, *scratchMesh);
            if (rebuilt) {
                outcome.addresses.push_back({.scratch = drawCmd.skinnedVertexBuffer, .address = *rebuilt});
            } else if (!outcome.failure.has_value()) {
                outcome.failure = rebuilt.error();
            }
        }

        Vk::MemoryBarrier(
            cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite, Vk::BarrierStage::AccelerationStructureBuild,
            Vk::BarrierAccess::AccelerationStructureRead
        );
    }
    return outcome;
}

void RenderContext::Impl::BuildTLAS(VkCommandBuffer cmd, std::span<const SkinnedBlasEntry> skinnedBlas) noexcept {
    if (!ctx.RayTracingSupported() || queues.Draws().empty()) {
        return;
    }

    tlasInstancesScratch.clear();
    tlasInstancesScratch.reserve(queues.Draws().size());

    using enum DrawFlags;

    for (uint32_t i = 0; i < queues.Draws().size(); ++i) {
        const auto& drawCmd = queues.Draws()[i];

        Vk::DeviceAddress<> blasAddress = drawCmd.blasAddress;
        if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
            blasAddress = nullptr;
            for (const auto& entry: skinnedBlas) {
                if (entry.scratch == drawCmd.skinnedVertexBuffer) {
                    blasAddress = entry.address;
                    break;
                }
            }
        }

        if (blasAddress == nullptr || ((drawCmd.flags & ExcludeFromTLAS) != None)) {
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
            // kRayMaskSkyboxLayer / kRayMaskWorld in uniforms.slang: the
            // background layer shares the TLAS with the main world but under
            // its own bit, so shadow rays can ignore it and the sky composite
            // can trace it alone behind the world's pixels.
            .mask                                   = ((drawCmd.flags & Background) != None) ? 0x01u : 0x02u,
            .instanceShaderBindingTableRecordOffset = 0,
            .flags                                  = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
            .accelerationStructureReference         = blasAddress.Get()
        };

        tlasInstancesScratch.push_back(inst);
    }

    if (tlasInstancesScratch.empty()) {
        return;
    }

    auto& instanceBuf = frames.tlasInstanceBuffers[presenter.frameIndex];

    auto mappedInstances = instanceBuf.Map(allocator);
    if (!mappedInstances)
        return;
    std::memcpy(mappedInstances->Data(), tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    Vk::TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    auto& scratch = frames.tlasScratchBuffer[presenter.frameIndex];
    Vk::BuildTLAS(
        cmd, geom, frames.tlas[presenter.frameIndex].Get(), Vk::BufferSlice {scratch, ctx.BufferAddress(scratch.Handle())}, tlasInstancesScratch.size()
    );

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite,
        Vk::BarrierStage::Fragment | Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead | Vk::BarrierAccess::AccelerationStructureRead
    );
}

void RenderContext::Impl::ApplySceneView(const SceneView& view) noexcept {
    const JPH::Mat44 unjittered = view.projMatrix * view.viewMatrix;

    current_view_proj    = view.viewProjMatrix;
    unjittered_view_proj = unjittered;

    currentUniforms.viewProj           = view.viewProjMatrix;
    currentUniforms.unjitteredViewProj = unjittered;
    currentUniforms.invViewProj        = unjittered.Inversed();
    currentUniforms.camPos             = JPH::Float4 {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time};
    if (view.frameIndex != 0) {
        currentUniforms.frameIndex = view.frameIndex;
    }

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
    gpu->frameIndex         = currentUniforms.frameIndex;
}

void RenderContext::Impl::PrepareSceneFrame(VkCommandBuffer cmd, const SceneView& view) noexcept {
    ApplySceneView(view);

    const SkinnedBlasOutcome skinnedBlas = DispatchSkinningPasses(cmd);
    if (skinnedBlas.failure.has_value() && diagnostics.invalidDrawWarningCount.fetch_add(1, std::memory_order::relaxed) < 5) {
        ZHLN::LogWarning("PrepareSceneFrame: skinned BLAS rebuild failed: {}", skinnedBlas.failure.value());
    }

    if (queues.Draws().size() > kGpuCullingMaxInstances) {
        queues.Draws().resize(kGpuCullingMaxInstances);
    }

    if (pendingLines.empty()) {
        activeLineVertexCount = 0;
    } else {
        const uint32_t maxVertices = std::min(static_cast<uint32_t>(pendingLines.size() * 2), kMaxLineVertices);

        activeLineVertexCount = 0;
        if (auto mapped = frames.lineVbos[presenter.frameIndex].Map(allocator)) {
            auto*    dst  = mapped->As<LineVertex>();
            uint32_t vIdx = 0;
            for (const auto& line: pendingLines) {
                if (vIdx + 2 > maxVertices) {
                    break;
                }

                dst[vIdx++] = {
                    .position = {line.start.GetX(), line.start.GetY(), line.start.GetZ()},
                    .color    = Math::PackColor(line.colorStart.GetX(), line.colorStart.GetY(), line.colorStart.GetZ(), line.colorStart.GetW())
                };
                dst[vIdx++] = {
                    .position = {line.end.GetX(), line.end.GetY(), line.end.GetZ()},
                    .color    = Math::PackColor(line.colorEnd.GetX(), line.colorEnd.GetY(), line.colorEnd.GetZ(), line.colorEnd.GetW())
                };
            }
            activeLineVertexCount = vIdx;
        }
    }
    pendingLines = {};
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
    BuildTLAS(cmd, skinnedBlas.addresses);
}

namespace {

template <typename Recorder, typename Scheduler, size_t... Is>
auto RecordForkBodies(Recorder& rec, Scheduler& scheduler, std::span<const Vk::ForkCall> bodies, std::index_sequence<Is...>) noexcept
    -> std::expected<void, ErrorCode> {
    return rec.Record(scheduler, ([&body = bodies[Is]](Vk::RecordingSlot slot) noexcept { body(slot.cmd); })...);
}

} // namespace

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

    const auto samplerBind  = self.heapManager.GetSamplerHeapBindInfo();
    const auto resourceBind = self.heapManager.GetResourceHeapBindInfo();
    const auto frameAddrs   = self.FrameHeapAddresses();
    rec.SetHeapState(
        &samplerBind, &resourceBind, GpuAbi::kScenePushLayout.UsedFrameAddresses(), std::span<const VkDeviceAddress> {frameAddrs.data(), frameAddrs.size()}
    );

    const bool previousInheritance  = self.frameState.inForkSecondary;
    self.frameState.inForkSecondary = true;

    TaskSystemScheduler            scheduler;
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

auto RenderContext::BeginFrame() noexcept -> FrameOutcome<FrameScope> {
    auto&          primarySync = _impl->presenter.sync;
    const uint32_t slot        = _impl->presenter.frameIndex;

    for (const uint32_t frameSlot: {Vk::PreviousFrameSlot(slot), slot}) {
        if (const VkResult waited = primarySync.Wait(frameSlot); waited != VK_SUCCESS) {
            return std::unexpected(Vk::ToError(waited));
        }

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
    _impl->diagnostics.gpuProfiler.RetrieveResults(frame_index, timestampPeriod, [](std::string_view name, float durationMS) -> void {
        CPUProfiler::Record(name, durationMS);
    });

    _impl->diagnostics.gpuProfiler.RetrievePipelineStats(frame_index, [this](std::string_view, const Profiler::PipelineStats& stats) -> void {
        auto& acc = _impl->diagnostics.pendingPipelineCounters;
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

    _impl->diagnostics.gpuProfiler.Reset(frame_index);
    _impl->computePools[frame_index].Reset();
    _impl->parallelRecorders[frame_index].Reset();

    for (auto& worker: _impl->workerCmds) {
        worker.cmdCount[frame_index].store(0, std::memory_order::relaxed);
        worker.pools[frame_index].Reset();
    }

    _impl->destinations.BeginFrame();
    ++_impl->frameSerial;
    _impl->EvictInactiveEmitters();

    _impl->billboardSerial = 0;
    _impl->frameState.Reset();
    _impl->sceneTarget.reset();

    auto& resized = _impl->frameState.resized;
    if (resized) {
        auto fbSize = GetFramebufferSize();
        if (!fbSize.has_value()) {
            return std::optional<FrameScope> {};
        }

        VkExtent2D ext = {.width = fbSize->width, .height = fbSize->height};

        if (!_impl->RecreateTargets(ext)) {
            return std::unexpected(FrameResult::TargetRecreationFailed);
        }

        resized = false;
    }

    return FrameScope {*this};
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

    std::swap(_impl->graphResources.shadowMap, _impl->targets.ShadowMapPrev());
    _impl->targets.SwapCascadeViews();
    std::swap(_impl->graphResources.voxelHistory, _impl->graphResources.voxelResolved);
    if (_impl->ctx.RayTracingSupported()) {
        std::swap(_impl->graphResources.rtShadowMask, _impl->graphResources.rtShadowHistory);
        std::swap(_impl->graphResources.rtrHalf, _impl->graphResources.rtrHistory);
        std::swap(_impl->graphResources.rtgiHalf, _impl->graphResources.rtgiHistory);
        _impl->rtHistoryValid = _impl->settings.rayTracing.denoiserPasses > 0;
    }

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

auto RenderContext::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) -> std::expected<TextureHandle, ErrorCode> {
    return _impl->CreateRenderTexture(width, height, hdr);
}

void RenderContext::DestroyRenderTexture(TextureHandle handle) noexcept {
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
    if (_impl->settings.post.enableRTR != settings.post.enableRTR || _impl->settings.rayTracing.enableReflections != settings.rayTracing.enableReflections ||
        _impl->settings.rayTracing.enableShadows != settings.rayTracing.enableShadows ||
        _impl->settings.rayTracing.enableDiffuseGI != settings.rayTracing.enableDiffuseGI ||
        _impl->settings.rayTracing.denoiserPasses != settings.rayTracing.denoiserPasses || settings.rayTracing.denoiserPasses == 0) {
        _impl->rtHistoryValid = false;
    }
    _impl->settings = settings;

    Pipelines::DeferredPbrPipeline::Execute(*_impl, cmd, view, settings);

    _impl->sceneTarget.reset();
    resolved->layout                         = Vk::AttachmentLayout::ColorAttachment;
    resolved->drawn                          = true;
    _impl->diagnostics.warnedUnwrittenTarget = false;
    return std::nullopt;
}

auto RenderContext::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped> {
    return Pipelines::UIPipeline::Execute(*_impl, view, uiData);
}

auto RenderContext::DispatchSimulations(float dt) noexcept -> RenderResult {
    return Pipelines::ComputeSimPipeline::Submit(*_impl, dt);
}

void RenderContext::ApplySimulationData(const SceneData& scene) noexcept {
    for (const ParticleEmitterSubmission& em: scene.particleEmitters) {
        SubmitParticleEmitter(em.emitterId, em.maxParticles, em.desc, em.texture, em.additive);
    }
    for (const MeshParticleEmitterSubmission& em: scene.meshParticleEmitters) {
        SubmitMeshParticleEmitter(em.emitterId, em.maxParticles, em.desc, em.mesh, em.mat);
    }
}

void RenderContext::ApplyDrawData(const SceneData& scene) noexcept {
    _impl->pendingLines = std::span<const LineSegment> {scene.lines.data(), scene.lines.size()};
    for (const BillboardBatch& batch: scene.billboards) {
        DrawBillboards(batch.texture, std::span<const BillboardQuad> {batch.quads.data(), batch.quads.size()}, batch.additive);
    }
    if (!scene.debugTriPositions.empty()) {
        constexpr MaterialID kPhysicsDebugMaterialID = HashAssetID("builtin_physics_debug_solid_material");
        Material             mat {};
        if (auto existing = GetGPUMaterial(kPhysicsDebugMaterialID)) {
            mat = *existing;
        } else if (auto created = CreateMaterial(MaterialDesc::Transparent({1.0f, 1.0f, 1.0f, 1.0f}, 0.1f, true))) {
            mat = *created;
            RegisterGPUMaterial(kPhysicsDebugMaterialID, mat);
        } else {
            return;
        }
        DrawDebugTriangles(
            std::span<const VertexPosition> {scene.debugTriPositions.data(), scene.debugTriPositions.size()},
            std::span<const VertexSurface> {scene.debugTriSurfaces.data(), scene.debugTriSurfaces.size()}, mat
        );
    }
}

void RenderContext::AbortFrame() noexcept {
    if (!_impl) {
        return;
    }
    _impl->destinations.AbortRecordings();
    _impl->frameOpen    = false;
    _impl->pendingLines = {};
    _impl->queues.Clear();
    _impl->frameState.Reset();
    _impl->sceneTarget.reset();
    _impl->destinations.SetActive(0);
}

FrameScope::FrameScope(RenderContext& rc) noexcept: _rc(&rc) {
}

FrameScope::FrameScope(FrameScope&& other) noexcept: _rc(other._rc), _ended(other._ended) {
    other._rc    = nullptr;
    other._ended = true;
}

auto FrameScope::operator=(FrameScope&& other) noexcept -> FrameScope& {
    if (this == &other) {
        return *this;
    }
    if (_rc != nullptr && !_ended) {
        _rc->AbortFrame();
    }
    _rc          = other._rc;
    _ended       = other._ended;
    other._rc    = nullptr;
    other._ended = true;
    return *this;
}

FrameScope::~FrameScope() {
    Cancel();
}

void FrameScope::Cancel() noexcept {
    if (_rc != nullptr && !_ended) {
        _rc->AbortFrame();
        _ended = true;
    }
}

auto FrameScope::DispatchSimulations(const SceneData& scene, float dt) noexcept -> RenderResult {
    if (_rc == nullptr) {
        return {};
    }
    _rc->ApplySimulationData(scene);
    return _rc->DispatchSimulations(dt);
}

auto FrameScope::RenderScene(const SceneRenderPass& pass, const SceneData& scene) noexcept -> FrameOutcome<FrameSkipped> {
    if (_rc == nullptr) {
        return FrameSkipped {};
    }
    _rc->SetFrameData(pass.camera, pass.frame, pass.shadowProjView, std::span<const LightDesc> {scene.lights.data(), scene.lights.size()}, scene.dt);
    _rc->ApplyDrawData(scene);
    return _rc->RenderScene(pass.view, pass.settings);
}

auto FrameScope::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped> {
    if (_rc == nullptr) {
        return FrameSkipped {};
    }
    return _rc->RenderUI(view, uiData);
}

auto FrameScope::End() && noexcept -> FrameOutcome<PresentSuboptimal> {
    if (_rc == nullptr || _ended) {
        return {};
    }
    _ended = true;
    return _rc->EndFrame();
}

auto RenderContext::Impl::ProvokeDeviceLostInternal() const -> std::expected<void, ErrorCode> {
    if (!compute.hangGpu.pipeline.Valid()) {
        return {};
    }

    if (ctx.PhysicalInfo().properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        ZHLN::Log(
            "[GPU] Skipping hang-GPU dispatch on CPU Vulkan device '{}'; it would SIGSEGV a worker thread.", ctx.PhysicalInfo().properties.properties.deviceName
        );
        return {};
    }

    if (const VkCommandBuffer cmd = FrameCommand(); cmd != VK_NULL_HANDLE) {
        compute.hangGpu.Bind(cmd);
        compute.hangGpu.DispatchGroups(cmd, 1, 1, 1);
    } else {
        if (auto immediate = Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto immediateCmd) -> auto {
            compute.hangGpu.Bind(immediateCmd);
            compute.hangGpu.DispatchGroups(immediateCmd, 1, 1, 1);
        }); !immediate) [[unlikely]] {
            return std::unexpected(immediate.error());
        }
    }
    return {};
}

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "OpenGLHacks/HostBlit.hpp"
#include "Zahlen/Profiler.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <atomic>
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

inline auto MapFrameResult(ZHLN_FrameResult res) noexcept -> RenderFrameResult {
    using enum RenderFrameResult;
    switch (res) {
        case ZHLN_FrameResult_Ok:
            return Success;
        case ZHLN_FrameResult_Suboptimal:
            return Suboptimal;
        case ZHLN_FrameResult_OutOfDate:
            return OutOfDate;
        case ZHLN_FrameResult_DeviceLost:
            return DeviceLost;
        default:
            return Error;
    }
}

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
        ctx.BufferAddress(frames.frameUniformBuffers[frame_index].Handle()), ctx.BufferAddress(frames.lightStorageBuffers[frame_index].Handle()),
        ctx.BufferAddress(frames.instanceDataBuffers[frame_index].Handle()), ctx.BufferAddress(frames.jointBuffers[frame_index].Handle()),
        ctx.BufferAddress(frames.jointBuffers[frame_index ^ 1].Handle()),    ctx.BufferAddress(morphDeltasBuffer.Handle()),
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

    auto& instanceBuf = frames.tlasInstanceBuffers[frame_index];

    // The instance buffer is host-visible and coherent (CPU_TO_GPU): write it
    // directly while recording. The memcpy completes before submission, and
    // the double-buffered parity means the previous TLAS build against this
    // buffer finished frames ago -- no staging copy, no transfer barrier.
    std::memcpy(instanceBuf.Map().data, tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    ZHLN_TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    rtCtx.BuildTLAS(cmd, geom, frames.tlas[frame_index], ctx.BufferAddress(frames.tlasScratchBuffer[frame_index].Handle()), tlasInstancesScratch.size());

    Vk::MemoryBarrier(
        cmd, Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureWrite,
        Vk::BarrierStage::Fragment, Vk::BarrierAccess::ShaderRead
    );
}

auto RenderContext::BeginFrame() noexcept -> RenderResult {
    using enum RenderFrameResult;

    // 1. Wait for the previous frame at this slot to finish
    auto wait_res = _impl->sync.Wait(_impl->frame_index ^ 1);
    if (wait_res == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(DeviceLost);
    }
    // Extra PresentViewports records UI into frames.uiVbos[frame_index] after
    // EndFrame flipped the slot. Tick's SubmitUI writes that same slot — wait
    // extra blits out before the CPU overwrites those vertices.
    for (auto& vp: _impl->viewports) {
        if (vp.sync.Wait(vp.frameIndex ^ 1u) == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(DeviceLost);
        }
    }

    auto& stagingContext = _impl->stagingContext;
    auto& frame_index    = _impl->frame_index;
    auto& deletionQueue  = _impl->deletionQueue;
    if (stagingContext) {
        stagingContext->Wait();
        stagingContext.reset();
    }

    deletionQueue.BeginFrame(frame_index);
    _impl->activeQueueGuard.emplace(deletionQueue);

    // Retrieve GPU profiling results
    float timestampPeriod = _impl->ctx.PhysicalInfo().properties.properties.limits.timestampPeriod;
    _impl->gpuProfiler.RetrieveResults(frame_index, timestampPeriod, [](std::string_view name, float durationMS) -> void {
        CPUProfiler::Record(name, durationMS);
    });

    _impl->sync.StepTimeline(frame_index);

    // Reset query pools
    _impl->gpuProfiler.Reset(frame_index);
    _impl->computePools[frame_index].Reset();

    for (auto& worker: _impl->workerCmds) {
        worker.cmdCount[frame_index].store(0, std::memory_order::relaxed);
        worker.pools[frame_index].Reset();
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

        _impl->needsInitialClear = true;
        resized                  = false;
    }

    _impl->current_cmd = reinterpret_cast<VkCommandBuffer>(1);
    return {};
}

void RenderContext::Impl::RecordIndirectTelemetry(VkCommandBuffer cmd) noexcept {
    if (!indirectReadbackReady) {
        bool ok = true;
        for (uint32_t i = 0; i < 2; ++i) {
            auto rb = Vk::Buffer::Create(allocator.Get(), kTelemetryReadbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
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
        cmd, frames.indirectCommandsBuffers[frame_index], Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::TransferRead
    );
    Vk::BufferBarrier(
        cmd, frames.indirectCommandsBuffersPass2[frame_index], Vk::BarrierStage::Compute | Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferRead
    );
    Vk::BufferBarrier(
        cmd, frames.secondPassCountBuffers[frame_index], Vk::BarrierStage::Compute | Vk::BarrierStage::Transfer,
        Vk::BarrierAccess::ShaderWrite | Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::Transfer, Vk::BarrierAccess::TransferRead
    );

    auto& dst = indirectReadbackBuffers[frame_index];
    Vk::CopyBuffer(cmd, frames.indirectCommandsBuffers[frame_index], dst, bytes, 0, kTelemetryPass1Offset);
    Vk::CopyBuffer(cmd, frames.indirectCommandsBuffersPass2[frame_index], dst, bytes, 0, kTelemetryPass2Offset);
    Vk::CopyBuffer(cmd, frames.secondPassCountBuffers[frame_index], dst, sizeof(uint32_t), 0, kTelemetryCountOffset);
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
        auto        mapped = indirectReadbackBuffers[frame_index].Map();
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

void RenderContext::Impl::RecordWindowFrame(VkCommandBuffer cmd, uint32_t imageIndex) noexcept {
    current_cmd         = cmd;
    current_image_index = imageIndex;

    pendingAcquires.Drain(cmd);
    DispatchSkinningPasses();

    if (queues.drawQueue.size() > kGpuCullingMaxInstances) {
        queues.drawQueue.resize(kGpuCullingMaxInstances);
    }

    FlushLineQueue();
    SortDrawQueue();

    auto drawCount = queues.drawQueue.size();
    auto csgCount  = queues.csgDrawQueue.size();

    if (drawCount > 0 || csgCount > 0) {
        auto  mapped = frames.instanceDataBuffers[frame_index].Map();
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

    RecordSceneFrame({cmd});

    if (Diag::IndirectTelemetryEnabled()) {
        RecordIndirectTelemetry(cmd);
    }
}

void RenderContext::Impl::RecordViewportPresent(VkCommandBuffer cmd, uint32_t imageIndex) noexcept {
    current_cmd         = cmd;
    current_image_index = imageIndex;

    auto&       dest = Presenting();
    const auto& sc   = dest.swapchain.Get();
    // Fresh extra command buffer: acquire leaves the image UNDEFINED (first
    // use) or PRESENT_SRC_KHR. LOAD_OP_DONT_CARE, so UNDEFINED as oldLayout
    // is the graph's swapchain ColorWrite barrier.
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
        cmd, sc.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT
    );
    Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> target {
        .handle = sc.images[imageIndex],
        .view   = sc.views[imageIndex],
        .extent = {.width = sc.extent.width, .height = sc.extent.height, .depth = 1},
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .format = sc.format,
    };

    FrameRecorder  blitRecorder(cmd, *this);
    const int      fullBright = currentUniforms.fullBright != 0 ? 1 : 0;
    const uint32_t fIdx       = frame_index;

    if (settings.antiAliasing.mode != AAMode::None) {
        auto& src = frames.accumBuffers.Current();
        blitPass.WriteHeap(
            ctx, heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_AccumNext>>(src), defaultSampler,
            Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(graphResources.bloomFinalTarget), Vk::Assume<Vk::ShaderRead<Res_Depth>>(presentation.depthTarget),
            frames.frameUniformBuffers[fIdx]
        );
        Passes::BlitPass {}.Execute(blitRecorder, Vk::Assume<Vk::ShaderRead<Res_AccumNext>>(src), target, fullBright);
    } else {
        auto& src = graphResources.hdrSceneColor;
        blitPass.WriteHeap(
            ctx, heapManager, fIdx, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(src), defaultSampler,
            Vk::Assume<Vk::ShaderRead<Res_BloomFinal>>(graphResources.bloomFinalTarget), Vk::Assume<Vk::ShaderRead<Res_Depth>>(presentation.depthTarget),
            frames.frameUniformBuffers[fIdx]
        );
        Passes::BlitPass {}.Execute(blitRecorder, Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(src), target, fullBright);
    }
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
                impl->current_cmd         = VK_NULL_HANDLE;
                impl->hasSkinnedThisFrame = false;
            }
        }
        EndFrameGuard(const EndFrameGuard&)                    = delete;
        auto operator=(const EndFrameGuard&) -> EndFrameGuard& = delete;
        EndFrameGuard(EndFrameGuard&&)                         = delete;
        auto operator=(EndFrameGuard&&) -> EndFrameGuard&      = delete;
    } frameGuard {_impl.get()};

    using enum RenderFrameResult;

    ZHLN_FrameResult res = ZHLN_FrameResult_Ok;

    {
        ZHLN::ScopedTimer profTimer("Render (CPU Record)");
        if (_impl->current_cmd == VK_NULL_HANDLE) {
            return std::unexpected(Error);
        }

        // ====================================================================
        // 1. RECORD & SUBMIT COMPUTE QUEUE (Async Compute Phase)
        // ====================================================================
        // VK_EXT_descriptor_heap: reset the per-frame dynamic region budget.
        _impl->heapManager.BeginFrame(_impl->frame_index);

        _impl->current_compute_cmd = _impl->computePools[_impl->frame_index][0];

        _impl->RecordComputeFrame(_impl->current_compute_cmd);

        uint64_t computeSignalValue = _impl->sync.GetTimelineValue(_impl->frame_index);

        auto comp_submit_res = Vk::QueueSubmit(
            _impl->ctx, _impl->current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, _impl->sync[_impl->frame_index].compute_timeline,
            computeSignalValue, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        );

        if (!comp_submit_res) [[unlikely]] {
            if (comp_submit_res.error().Is<VkResult>() && comp_submit_res.error().As<VkResult>() == VK_ERROR_DEVICE_LOST) {
                Vk::Instance::NotifyDeviceLost();
                return std::unexpected(DeviceLost);
            }
            return std::unexpected(comp_submit_res.error());
        }

        // ====================================================================
        // 2. RECORD & SUBMIT GRAPHICS QUEUE
        // ====================================================================
        if (_impl->presentation.swapchain.Get().handle == VK_NULL_HANDLE) {
            // ================================================================
            // HEADLESS PATH: No swapchain. Record and submit directly.
            // ================================================================
            const auto cmd     = _impl->pools.Cmd(_impl->frame_index);
            _impl->current_cmd = cmd;

            _impl->sync.ResetFence(_impl->frame_index);
            _impl->pools[_impl->frame_index].Reset();

            // RAII command-buffer scope: begin on construction, end on exit.
            //
            // The scope below is LOAD-BEARING. Without it the guard lives until the
            // end of the enclosing block, i.e. past the vkQueueSubmit2 below, and the
            // primary is submitted while still in the recording state:
            //   VUID-vkQueueSubmit2-commandBuffer-03874, once per frame, on every
            //   headless frame (which is every GPU test).
            {
                Vk::CommandBufferGuard recordGuard(cmd);

                _impl->pendingAcquires.Drain(cmd);
                _impl->DispatchSkinningPasses();

                if (_impl->queues.drawQueue.size() > kGpuCullingMaxInstances) {
                    _impl->queues.drawQueue.resize(kGpuCullingMaxInstances);
                }
                _impl->FlushLineQueue();

                _impl->SortDrawQueue();

                auto drawCount = _impl->queues.drawQueue.size();
                auto csgCount  = _impl->queues.csgDrawQueue.size();

                if (drawCount > 0 || csgCount > 0) {
                    auto  mapped = _impl->frames.instanceDataBuffers[_impl->frame_index].Map();
                    auto* dst    = static_cast<InstanceData*>(mapped.data);

                    for (size_t i = 0; i < drawCount; ++i) {
                        dst[i] = _impl->queues.drawQueue[i].instanceData;
                    }

                    uint32_t csgOffset = drawCount;
                    for (auto& csgCmd: _impl->queues.csgDrawQueue) {
                        dst[csgOffset]        = csgCmd.eyeDraw.instanceData;
                        csgCmd.eyeInstanceIdx = csgOffset++;

                        for (auto& cutter: csgCmd.cutters) {
                            dst[csgOffset]     = cutter.draw.instanceData;
                            cutter.instanceIdx = csgOffset++;
                        }
                    }
                }
                _impl->BuildTLAS(cmd);

                if (Diag::IndirectTelemetryEnabled()) {
                    static uint32_t s_TelemetryFrame = 0;
                    ++s_TelemetryFrame;
                    if (s_TelemetryFrame >= 4 && (s_TelemetryFrame % 120) == 4) {
                        _impl->DumpIndirectTelemetry(s_TelemetryFrame);
                    }
                }

                _impl->RecordSceneFrame({cmd});

                if (Diag::IndirectTelemetryEnabled()) {
                    _impl->RecordIndirectTelemetry(cmd);
                }
            } // recordGuard destructor ends the command buffer HERE, before the submit.

            // Submit directly to the graphics queue with timeline semaphore sync.
            // Wait on the compute timeline (same as the windowed path) and signal
            // the in-flight fence so BeginFrame can wait on it next frame.
            auto submit_res = Vk::QueueSubmit(
                _impl->ctx.GraphicsQueue(), static_cast<VkCommandBuffer>(cmd), _impl->sync[_impl->frame_index].compute_timeline, computeSignalValue,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, _impl->sync[_impl->frame_index].in_flight
            );

            if (!submit_res) {
                if (submit_res.error().Is<VkResult>() && submit_res.error().As<VkResult>() == VK_ERROR_DEVICE_LOST) {
                    Vk::Instance::NotifyDeviceLost();
                    return std::unexpected(DeviceLost);
                }
                return std::unexpected(Error);
            }

            // HostBlit (macOS): the finished frame now lives in the offscreen
            // headlessColorTarget. The plugin copies it out on its own fence
            // (which also waits on the submit above) and blits it through its
            // own OpenGL window. Closing that window ends the session, just
            // like closing any other engine window.
            if constexpr (isMac) {
                if (_impl->presentationMode == PresentationMode::HostBlit) {
                    const auto& target = _impl->presentation.headlessColorTarget;
                    if (target.Valid()) {
                        auto* win = static_cast<GLFWwindow*>(_impl->window.GetNativeHandle());
                        if (!HostBlit::Present(
                                target.image, win, target.extent.width, target.extent.height,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            )) {
                            _impl->window.Close();
                        }
                    }
                }
            }

            // Advance the frame index
            _impl->frame_index = (_impl->frame_index + 1) & 1;
        } else {
            // ================================================================
            // WINDOWED / TTY PATH: scene graph once, on the primary swapchain.
            // ================================================================
            _impl->presenting = &_impl->presentation;
            res = Vk::DrawFrame<2, false>(
                {.ctx               = _impl->ctx,
                 .swapchain         = _impl->presentation.swapchain,
                 .sync              = _impl->sync,
                 .pools             = _impl->pools,
                 .presentSemaphores = _impl->presentation.presentSemaphores,
                 .stagingSemaphore  = _impl->transferRingBuffer.GetSemaphore(),
                 .stagingWaitValue  = _impl->transferRingBuffer.GetCurrentValue(),
                 .computeSemaphore  = _impl->sync[_impl->frame_index].compute_timeline,
                 .computeWaitValue  = computeSignalValue},
                _impl->frame_index,
                [this](VkCommandBuffer cmd, uint32_t image_index) -> void { _impl->RecordWindowFrame(cmd, image_index); },
                [this]() -> void { _impl->resized = true; }
            );

            if (res != ZHLN_FrameResult_Ok && res != ZHLN_FrameResult_Suboptimal) {
                _impl->presenting = nullptr;
                return std::unexpected(MapFrameResult(res));
            }
            _impl->presenting = nullptr;
        }

        // Flip double-buffered resources
        _impl->frames.FlipAll();

        std::swap(_impl->graphResources.shadowMap, _impl->shadowMapPrev);
        std::swap(_impl->shadowCascadeViews, _impl->shadowCascadeViewsPrev);
        std::swap(_impl->graphResources.voxelHistory, _impl->graphResources.voxelResolved);
    }

    if (res == ZHLN_FrameResult_Suboptimal) {
        return std::unexpected(Suboptimal);
    }

    return {};
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

auto RenderContext::Impl::DestroyViewports() noexcept -> std::expected<void, Error> {
    if (viewports.empty()) {
        return {};
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        viewports.clear();
        return std::unexpected(Vk::PresentationError::ContextInvalid);
    }
    auto idle = Vk::WaitIdle(ctx.Device());
    viewports.clear();
    return idle;
}

auto RenderContext::Impl::RemoveViewport(Window& aux) noexcept -> std::expected<void, Error> {
    const auto it = std::find_if(viewports.begin(), viewports.end(), [&](const Viewport& vp) { return vp.window == &aux; });
    if (it == viewports.end()) {
        return {};
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        viewports.erase(it);
        return std::unexpected(Vk::PresentationError::ContextInvalid);
    }
    auto idle = Vk::WaitIdle(ctx.Device());
    viewports.erase(it);
    return idle;
}

auto RenderContext::Impl::AddViewport(Window& aux) noexcept -> std::expected<void, Error> {
    using Vk::PresentationError;
    using Vk::SurfaceCreationError;

    if (presentationMode != PresentationMode::NativeSwapchain) {
        return std::unexpected(PresentationError::NativeSwapchainRequired);
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        return std::unexpected(PresentationError::ContextInvalid);
    }
    if (&aux == &window) {
        return std::unexpected(PresentationError::PrimaryWindowAlreadyPresented);
    }
    for (const auto& vp: viewports) {
        if (vp.window == &aux && vp.presentation.swapchain.Valid()) {
            return {};
        }
    }
    if (auto removed = RemoveViewport(aux); !removed) {
        return std::unexpected(removed.error());
    }

    int  width  = 0;
    int  height = 0;
    auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
    if (!surfaceRes) {
        return std::unexpected(surfaceRes.error());
    }

    Viewport vp;
    vp.surface = Vk::Surface(ctx.Instance(), static_cast<VkSurfaceKHR>(*surfaceRes));
    if (vp.surface.Get() == VK_NULL_HANDLE || width <= 0 || height <= 0) {
        return std::unexpected(SurfaceCreationError::WindowSurfaceCreationFailed);
    }
    if (auto initRes = vp.presentation.Init(ctx, allocator, vp.surface.Get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), true); !initRes) {
        return std::unexpected(initRes.error());
    }
    if (vp.presentation.GetPresentFormat() != presentation.GetPresentFormat()) {
        return std::unexpected(PresentationError::PresentFormatMismatch);
    }

    vp.sync = Vk::FrameSync<2>::Create(ctx.Device());
    vp.pools = Vk::CommandPools<2, Vk::QueueType::Graphics>::Create(
        ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1}
    );
    vp.frameIndex = 0;
    if (!vp.sync.Valid() || !vp.pools.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }

    vp.window = &aux;
    viewports.push_back(std::move(vp));
    return {};
}

auto RenderContext::Impl::WaitViewports() noexcept -> std::expected<void, Error> {
    using enum RenderFrameResult;
    for (auto& vp: viewports) {
        if (vp.sync.Wait(vp.frameIndex ^ 1u) == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(DeviceLost);
        }
    }
    return {};
}

auto RenderContext::Impl::PresentViewports() noexcept -> std::expected<void, Error> {
    using enum RenderFrameResult;

    struct UiQueueGuard {
        RenderQueues& queues;
        ~UiQueueGuard() noexcept {
            queues.uiBatches.clear();
        }
    } uiGuard {queues};

    if (viewports.empty()) {
        return {};
    }
    if (sync.Wait(frame_index ^ 1u) == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(DeviceLost);
    }

    Viewport* previous = nullptr;
    for (auto& vp: viewports) {
        if (vp.window == nullptr || !vp.window->IsRunning() || !vp.presentation.swapchain.Valid()) {
            continue;
        }
        const Extent2D size = vp.window->GetSize();
        if (size.width == 0 || size.height == 0) {
            continue;
        }
        const VkExtent2D scExtent = vp.presentation.swapchain.Get().extent;
        if (size.width != scExtent.width || size.height != scExtent.height) {
            if (auto rebuilt = vp.presentation.Rebuild(size.width, size.height); !rebuilt) {
                return std::unexpected(rebuilt.error());
            }
        }
        if (previous != nullptr && previous->sync.Wait(previous->frameIndex ^ 1u) == VK_ERROR_DEVICE_LOST) {
            return std::unexpected(DeviceLost);
        }

        presenting                             = &vp.presentation;
        std::expected<void, ZHLN::Error> rebuilt {};
        const ZHLN_FrameResult           extraRes = Vk::DrawFrame<2>(
            {.ctx               = ctx,
             .swapchain         = vp.presentation.swapchain,
             .sync              = vp.sync,
             .pools             = vp.pools,
             .presentSemaphores = vp.presentation.presentSemaphores},
            vp.frameIndex,
            [this](VkCommandBuffer cmd, uint32_t imageIndex) -> void { RecordViewportPresent(cmd, imageIndex); },
            [&]() -> void { rebuilt = vp.presentation.Rebuild(size.width, size.height); }
        );
        presenting = nullptr;
        if (!rebuilt) {
            return std::unexpected(rebuilt.error());
        }
        switch (extraRes) {
            case ZHLN_FrameResult_Ok:
            case ZHLN_FrameResult_Suboptimal:
                break;
            case ZHLN_FrameResult_OutOfDate:
                return std::unexpected(OutOfDate);
            case ZHLN_FrameResult_DeviceLost:
                return std::unexpected(DeviceLost);
            case ZHLN_FrameResult_Error:
                return std::unexpected(Error);
        }
        previous = &vp;
    }
    return {};
}

auto RenderContext::AddViewport(Window& window) noexcept -> RenderResult {
    return _impl->AddViewport(window);
}

auto RenderContext::RemoveViewport(Window& window) noexcept -> RenderResult {
    return _impl->RemoveViewport(window);
}

auto RenderContext::PresentViewports() noexcept -> RenderResult {
    return _impl->PresentViewports();
}

} // namespace ZHLN

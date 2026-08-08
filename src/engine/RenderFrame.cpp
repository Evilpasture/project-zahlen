// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp"
#include "engine/Scheduler.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN {

namespace {

inline RenderFrameResult MapFrameResult(ZHLN_FrameResult res) noexcept {
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
[[nodiscard]] constexpr bool AnyNull(Ptrs... ptrs) noexcept {
    return (... || (ptrs == nullptr));
}

} // namespace

// ============================================================================
// RenderContext Infrastructure & Lifecycles
// ============================================================================

std::optional<Extent2D> RenderContext::GetFramebufferSize() const {
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

            if (AnyNull(posMesh, attrMesh, skinMesh, scratchMesh)) {
                continue;
            }

            SkinningConstants pcs {
                .inPosAddr        = posMesh->vboAddress,
                .inAttrAddr       = attrMesh->vboAddress,
                .inSkinAddr       = skinMesh->vboAddress,
                .outPosAddr       = scratchMesh->vboAddress,
                .outAttrAddr      = scratchMesh->vboAddress + (scratchMesh->vertexCount * sizeof(VertexPosition)),
                .jointsAddr       = ctx.BufferAddress(jointBuffers->Handle()),
                .morphDeltasAddr  = ctx.BufferAddress(morphDeltasBuffer.Handle()),
                .vertexCount      = posMesh->vertexCount,
                .jointOffset      = drawCmd.jointOffset,
                .morphOffset      = drawCmd.morphOffset,
                .activeMorphCount = drawCmd.activeMorphCount,
                .morphWeights     = {drawCmd.morphWeights[0], drawCmd.morphWeights[1], drawCmd.morphWeights[2], drawCmd.morphWeights[3]}
            };

            skinningPass.PushConstants(cmd, pcs);
            Vk::ComputePass::Dispatch(cmd, (posMesh->vertexCount + 63) / 64, 1, 1);
        }
    }

    Vk::MemoryBarrier(
        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              .src_access = VK_ACCESS_2_SHADER_WRITE_BIT,
              .dst_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
              .dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT}
    );

    if (rtCtx.Valid()) {
        ZHLN::ScopedTimer profTimer("GPU Skinned BLAS Rebuilds");
        for (const auto& drawCmd: queues.drawQueue) {
            if (drawCmd.skinnedVertexBuffer != BufferHandle::Invalid) {
                auto* scratchMesh = meshPool.Resolve(drawCmd.skinnedVertexBuffer).value_or(nullptr);
                if (scratchMesh != nullptr) {
                    BuildOrUpdateSkinnedBLAS(cmd, drawCmd, scratchMesh);
                }
            }
        }

        Vk::MemoryBarrier(
            cmd, {.src_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  .src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  .dst_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                  .dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR}
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
            .transform =
                [&]() {
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

    auto& stagingBuf  = tlasStagingBuffers[frame_index];
    auto& instanceBuf = tlasInstanceBuffers[frame_index];

    std::memcpy(stagingBuf.Map().data, tlasInstancesScratch.data(), tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    Vk::CopyBuffer(cmd, stagingBuf, instanceBuf, tlasInstancesScratch.size() * sizeof(VkAccelerationStructureInstanceKHR));

    Vk::MemoryBarrier(
        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COPY_BIT,
              .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
              .dst_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              .dst_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT}
    );

    ZHLN_TlasGeometryDesc geom = {.instance_data = ctx.BufferAddress(instanceBuf.Handle())};

    rtCtx.BuildTLAS(cmd, geom, tlas[frame_index], ctx.BufferAddress(tlasScratchBuffer[frame_index].Handle()), tlasInstancesScratch.size());

    Vk::MemoryBarrier(
        cmd, {.src_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
              .src_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
              .dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
              .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
    );
}

RenderResult RenderContext::BeginFrame() noexcept {
    using enum RenderFrameResult;

    // 1. Wait for the previous frame at this slot to finish
    auto wait_res = _impl->sync.Wait(_impl->frame_index ^ 1);
    if (wait_res == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(DeviceLost);
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
    _impl->gpuProfiler.RetrieveResults(frame_index, timestampPeriod, [](std::string_view name, float durationMS) { CPUProfiler::Record(name, durationMS); });

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

RenderResult RenderContext::EndFrame() noexcept {
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
        EndFrameGuard(const EndFrameGuard&)            = delete;
        EndFrameGuard& operator=(const EndFrameGuard&) = delete;
        EndFrameGuard(EndFrameGuard&&)                 = delete;
        EndFrameGuard& operator=(EndFrameGuard&&)      = delete;
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
        _impl->current_compute_cmd = _impl->computePools[_impl->frame_index][0];

        _impl->RecordComputeFrame(_impl->current_compute_cmd);

        uint64_t computeSignalValue = _impl->sync.GetTimelineValue(_impl->frame_index);

        auto comp_submit_res = Vk::QueueSubmit(
            _impl->ctx, _impl->current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, _impl->sync[_impl->frame_index].compute_timeline,
            computeSignalValue, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        );

        if (!comp_submit_res) [[unlikely]] {
            return std::unexpected(comp_submit_res.error());
        }

        // ====================================================================
        // 2. RECORD GRAPHICS QUEUE
        // ====================================================================
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
            [this](VkCommandBuffer cmd, uint32_t image_index) {
                _impl->current_cmd         = cmd;
                _impl->current_image_index = image_index;

                _impl->pendingAcquires.Drain(cmd);

                _impl->DispatchSkinningPasses();

                if (_impl->queues.drawQueue.size() > kGpuCullingMaxInstances) {
                    _impl->queues.drawQueue.resize(kGpuCullingMaxInstances);
                }

                _impl->SortDrawQueue();

                auto drawCount = _impl->queues.drawQueue.size();
                auto csgCount  = _impl->queues.csgDrawQueue.size();

                if (drawCount > 0 || csgCount > 0) {
                    auto  mapped = _impl->instanceDataBuffers[_impl->frame_index].Map();
                    auto* dst    = static_cast<InstanceData*>(mapped.data);

                    // 1. Write standard draw queue
                    for (size_t i = 0; i < drawCount; ++i) {
                        dst[i] = _impl->queues.drawQueue[i].instanceData;
                    }

                    // 2. Write CSG draw queue
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

                // Graphics-only recording
                _impl->RecordSceneFrame({cmd});
            },
            [this]() { _impl->resized = true; }
        );

        if (res != ZHLN_FrameResult_Ok && res != ZHLN_FrameResult_Suboptimal) {
            return std::unexpected(MapFrameResult(res));
        }

        // Flip double-buffered resources
        ZHLN::Reflect::ForEachField(*_impl, [](auto& field) { FlipObject(field); });

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

    if (current_cmd != VK_NULL_HANDLE) {
        hangGpuPass.Bind(current_cmd);
        Vk::ComputePass::Dispatch(current_cmd, 512, 512, 1);
    } else {
        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](auto cmd) {
            hangGpuPass.Bind(cmd);
            hangGpuPass.Dispatch(cmd, 512, 512, 1);
        });
    }
}

} // namespace ZHLN

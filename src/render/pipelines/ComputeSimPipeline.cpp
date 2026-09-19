// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/ComputeSimPipeline.cpp

#include "ComputeSimPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

void ComputeSimPipeline::Submit(RenderContext::Impl& impl, float dt) noexcept {
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return;
    }

    const uint32_t slot = impl.presenter.frameIndex;

    impl.currentDt            = dt;
    impl.current_compute_cmd  = impl.computePools[slot][0];

    // 1. Record: cluster bounds, cluster culling, volumetric fog, particle
    //    updates -- the whole compute graph, on the frame's compute schedule.
    impl.RecordComputeFrame(impl.current_compute_cmd);

    // 2. Submit on the async compute queue, signalling the frame's timeline.
    //    The graphics submit waits on that value at kAsyncComputeConsumerStages
    //    (see PresentUsedWindows), which is what orders the two queues without a
    //    pipeline barrier across them.
    const uint64_t signalValue = impl.presenter.sync.GetTimelineValue(slot);
    auto           submitted =
        Vk::QueueSubmit(impl.ctx, impl.current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, impl.presenter.sync[slot].compute_timeline, signalValue, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    if (!submitted) [[unlikely]] {
        // QueueSubmit maps the submit call's result the one way the frame path
        // does, so "the device died" has a name here (and every other code is
        // the driver's own).
        if (submitted.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::NotifyDeviceLost();
        } else {
            ZHLN::Log("[DispatchCompute] Compute submission failed ({}).", submitted.error());
        }
        return;
    }

    impl.computeSubmittedThisFrame = true;
}

} // namespace ZHLN::Pipelines

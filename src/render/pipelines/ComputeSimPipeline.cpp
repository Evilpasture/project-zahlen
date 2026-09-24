// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/ComputeSimPipeline.cpp

#include "ComputeSimPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

auto ComputeSimPipeline::Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult {
    // Defensive only: Create() refuses a context without a device, so a live
    // context always has one. Nothing to dispatch, nothing failed.
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return {};
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
        Vk::QueueSubmit(impl.ctx, impl.current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, impl.presenter.sync.ComputeTimeline(slot), signalValue, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    if (!submitted) [[unlikely]] {
        // QueueSubmit maps the submit call's result the one way the frame path
        // does, so "the device died" has a name here (and every other code is
        // the driver's own). The increment is diagnostics; the recovery lever
        // is the error return -- the frame's caller propagates it this frame
        // instead of finding out one frame late at a fence wait.
        if (submitted.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::IncrementNumericalDeviceLoss();
        } else {
            ZHLN::Log("[DispatchSimulations] Compute submission failed ({}).", submitted.error());
        }
        return std::unexpected(submitted.error());
    }

    impl.frameState.computeSubmitted = true;
    return {};
}

} // namespace ZHLN::Pipelines

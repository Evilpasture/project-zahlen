// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "ComputeSimPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

auto ComputeSimPipeline::Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult {
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return {};
    }

    const uint32_t slot = impl.presenter.frameIndex;

    impl.currentDt            = dt;
    impl.current_compute_cmd  = impl.computePools[slot][0];

    impl.RecordComputeFrame(impl.current_compute_cmd);

    const uint64_t signalValue = impl.presenter.sync.GetTimelineValue(slot);
    auto           submitted =
        Vk::QueueSubmit(impl.ctx, impl.current_compute_cmd, VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE, impl.presenter.sync.ComputeTimeline(slot), signalValue, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

    if (!submitted) [[unlikely]] {
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

}

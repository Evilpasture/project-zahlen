// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/TAAPass.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct TAAPushConstants {
    float feedback;
};
static_assert(GpuAbi::ScenePassPayload<TAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void TAAPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::Taa, Shaders::Modules::TaaPS>::Record(frame, ctx.Cmd(), aa.taa, [&]() {
        return MakeFullscreenPassArgs(
            TAAPushConstants {.feedback = frame.settings.antiAliasing.taaFeedback},
            Vk::Slot<"texCurrent">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(frame.graphResources.hdrSceneColor)),
            Vk::Slot<"texHistory">(Vk::Assume<Vk::ShaderRead<Res_AccumPrevious>>(frame.accumulationHistory.Previous())),
            Vk::Slot<"texVelocity">(Vk::Assume<Vk::ShaderRead<Res_Velocity>>(frame.graphResources.velocityBuffer)),
            Vk::Slot<"frame">(frame.frames.frameUniformBuffers[frame.presenter.frameIndex])
        );
    });
}

} // namespace ZHLN::Passes

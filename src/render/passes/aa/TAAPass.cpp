// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/TAAPass.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// How much of the resolved history survives into this frame. Low values track
// motion better and shimmer more; high values smear.
struct TAAPushConstants {
    float feedback;
};
static_assert(GpuAbi::ScenePassPayload<TAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void TAAPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::Taa, Shaders::Modules::TaaPS>::Record(
        impl, ctx.Cmd(), impl.taaPass, [&]() {
            const uint32_t fIdx = impl.presenter.frameIndex;

            return MakeFullscreenPassArgs(
                TAAPushConstants {.feedback = impl.settings.antiAliasing.taaFeedback},
                Vk::Slot<"texCurrent">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(impl.graphResources.hdrSceneColor)),
                Vk::Slot<"texHistory">(Vk::Assume<Vk::ShaderRead<Res_AccumPrevious>>(impl.accumulationHistory.Previous())),
                Vk::Slot<"texVelocity">(Vk::Assume<Vk::ShaderRead<Res_Velocity>>(impl.graphResources.velocityBuffer)),
                Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx])
            );
        }
    );
}

}

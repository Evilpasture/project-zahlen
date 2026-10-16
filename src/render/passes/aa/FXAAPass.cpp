// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/FXAAPass.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct FXAAPushConstants {
    float rcpFrameX;
    float rcpFrameY;
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
    float _pad;
};
static_assert(GpuAbi::ScenePassPayload<FXAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void FXAAPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::Fxaa, Shaders::Modules::FxaaPS>::Record(frame, ctx.Cmd(), aa.fxaa, [&]() {
        const auto& inputColor = frame.graphResources.hdrSceneColor;
        const auto  rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
        const auto  rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

        return MakeFullscreenPassArgs(
            FXAAPushConstants {
                .rcpFrameX        = rcpW,
                .rcpFrameY        = rcpH,
                .subpix           = frame.settings.antiAliasing.fxaaSubpix,
                .edgeThreshold    = frame.settings.antiAliasing.fxaaEdgeThreshold,
                .edgeThresholdMin = frame.settings.antiAliasing.fxaaEdgeThresholdMin,
                ._pad             = 0.0f
            },
            Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
        );
    });
}

} // namespace ZHLN::Passes

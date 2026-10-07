// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/FXAAPass.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// Reciprocal frame dimensions to turn a pixel into a UV, then the sub-pixel
// blend amount and the two edge-detection thresholds.
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
    FullscreenPassRecorder<Shaders::Fxaa, Shaders::Modules::FxaaPS>::Record(
        impl, ctx.Cmd(), impl.fxaaPass, [&]() {
            const auto& inputColor = impl.graphResources.hdrSceneColor;
            const float rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
            const float rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

            return MakeFullscreenPassArgs(
                FXAAPushConstants {
                    rcpW, rcpH, impl.settings.antiAliasing.fxaaSubpix, impl.settings.antiAliasing.fxaaEdgeThreshold,
                    impl.settings.antiAliasing.fxaaEdgeThresholdMin, 0.0f
                },
                Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
            );
        }
    );
}

}

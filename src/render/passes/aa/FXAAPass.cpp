// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/FXAAPass.hpp"
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
    if (impl.fxaaPass.pipeline.Valid()) {
        const auto& inputColor = impl.graphResources.hdrSceneColor;
        const float rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
        const float rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

        const Vk::HeapBlockBase block = impl.fxaaPass.WriteHeapParameters<Shaders::Fxaa>(
            impl.ctx, impl.heapManager, Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
        );

        impl.fxaaPass.ExecuteHeap<Shaders::Modules::FxaaPS>(
            impl.ctx, ctx.Cmd(),
            FXAAPushConstants {
                rcpW, rcpH, impl.settings.antiAliasing.fxaaSubpix, impl.settings.antiAliasing.fxaaEdgeThreshold,
                impl.settings.antiAliasing.fxaaEdgeThresholdMin, 0.0f
            },
            block
        );
    }
}

}

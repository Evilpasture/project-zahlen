// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/MLAAPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// Reciprocal frame dimensions, the luma difference that counts as an edge, and
// how far along an edge the search for its endpoints may walk.
struct MLAAPushConstants {
    float    rcpFrameX;
    float    rcpFrameY;
    float    threshold;
    uint32_t maxSearchSteps;
};
static_assert(GpuAbi::ScenePassPayload<MLAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void MLAAPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    if (impl.mlaaPass.pipeline.Valid()) {
        const auto& inputColor = impl.graphResources.hdrSceneColor;
        const float rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
        const float rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

        const Vk::HeapBlockBase block = impl.mlaaPass.WriteHeapParameters<Shaders::Mlaa>(
            impl.ctx, impl.heapManager, Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
        );

        impl.mlaaPass.ExecuteHeap<Shaders::Modules::MlaaPS>(
            impl.ctx, ctx.Cmd(),
            MLAAPushConstants {rcpW, rcpH, impl.settings.antiAliasing.mlaaThreshold, impl.settings.antiAliasing.mlaaMaxSearchSteps}, block
        );
    }
}

}

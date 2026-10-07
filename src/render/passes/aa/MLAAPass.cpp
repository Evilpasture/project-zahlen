// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/MLAAPass.hpp"
#include "passes/FullscreenPassRecorder.hpp"
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
    FullscreenPassRecorder<Shaders::Mlaa, Shaders::Modules::MlaaPS>::Record(
        impl, ctx.Cmd(), impl.mlaaPass, [&]() {
            const auto& inputColor = impl.graphResources.hdrSceneColor;
            const float rcpW       = 1.0f / static_cast<float>(inputColor.extent.width);
            const float rcpH       = 1.0f / static_cast<float>(inputColor.extent.height);

            return MakeFullscreenPassArgs(
                MLAAPushConstants {rcpW, rcpH, impl.settings.antiAliasing.mlaaThreshold, impl.settings.antiAliasing.mlaaMaxSearchSteps},
                Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
            );
        }
    );
}

}

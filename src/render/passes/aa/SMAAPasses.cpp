// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/SMAAPasses.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

namespace {

// Reciprocal and absolute viewport size in one payload: the technique is
// resolution-independent but its LUT lookups are expressed in pixels.
struct SmaaPushConstants {
    float rtMetrics[4];
};
static_assert(GpuAbi::ScenePassPayload<SmaaPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

[[nodiscard]] auto MetricsOf(VkExtent2D extent) noexcept -> SmaaPushConstants {
    return SmaaPushConstants {
        .rtMetrics = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height), static_cast<float>(extent.width),
                      static_cast<float>(extent.height)}
    };
}

} // namespace

void SmaaEdgePass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaEdge, Shaders::Modules::SmaaEdgeVS>::Record(
        impl, ctx.Cmd(), impl.smaaEdgePass, [&]() {
            const auto& inputColor = impl.graphResources.hdrSceneColor;

            return MakeFullscreenPassArgs(
                MetricsOf(inputColor.extent),
                Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
            );
        }
    );
}

void SmaaWeightPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaWeight, Shaders::Modules::SmaaWeightVS, Shaders::Modules::SmaaWeightPS>::Record(
        impl, ctx.Cmd(), impl.smaaWeightPass, [&]() {
            const auto& areaView   = impl.textureManager.View(impl.postProcess.SmaaAreaTexture());
            const auto& searchView = impl.textureManager.View(impl.postProcess.SmaaSearchTexture());

            return MakeFullscreenPassArgs(
                MetricsOf(impl.graphResources.smaaWeightTarget.extent),
                Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(impl.graphResources.smaaEdgeTarget)),
                Vk::Slot<"areaTex">(areaView),
                Vk::Slot<"searchTex">(searchView)
            );
        }
    );
}

void SmaaBlendPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaBlend, Shaders::Modules::SmaaBlendVS, Shaders::Modules::SmaaBlendPS>::Record(
        impl, ctx.Cmd(), impl.smaaBlendPass, [&]() {
            const auto& inputColor = impl.graphResources.hdrSceneColor;

            return MakeFullscreenPassArgs(
                MetricsOf(inputColor.extent),
                Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
                Vk::Slot<"blendTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(impl.graphResources.smaaWeightTarget))
            );
        }
    );
}

}

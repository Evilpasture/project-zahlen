// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/SMAAPasses.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/aa/AntiAliasingResources.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

namespace {

using SmaaPushConstants = Slang::SmaaPushConstants;
static_assert(GpuAbi::ScenePassPayload<SmaaPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

[[nodiscard]] auto MetricsOf(VkExtent2D extent) noexcept {
    return SmaaPushConstants {
        .rtMetrics = {
            1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height), static_cast<float>(extent.width),
            static_cast<float>(extent.height)
        }
    };
}

} // namespace

void SmaaEdgePass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaEdge, Shaders::Modules::SmaaEdgeVS>::Record(frame, ctx, aa.smaaEdge, [&]() {
        const auto& inputColor = frame.graphResources.hdrSceneColor;

        return MakeFullscreenPassArgs(MetricsOf(inputColor.extent), Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)));
    });
}

void SmaaWeightPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaWeight, Shaders::Modules::SmaaWeightVS, Shaders::Modules::SmaaWeightPS>::Record(frame, ctx, aa.smaaWeight, [&]() {
        const auto& areaView   = frame.textureManager.View(aa.smaaAreaTexIdx);
        const auto& searchView = frame.textureManager.View(aa.smaaSearchTexIdx);

        return MakeFullscreenPassArgs(
            MetricsOf(frame.graphResources.smaaWeightTarget.extent),
            Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(frame.graphResources.smaaEdgeTarget)), Vk::Slot<"areaTex">(areaView),
            Vk::Slot<"searchTex">(searchView)
        );
    });
}

void SmaaBlendPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    FullscreenPassRecorder<Shaders::SmaaBlend, Shaders::Modules::SmaaBlendVS, Shaders::Modules::SmaaBlendPS>::Record(frame, ctx, aa.smaaBlend, [&]() {
        const auto& inputColor = frame.graphResources.hdrSceneColor;

        return MakeFullscreenPassArgs(
            MetricsOf(inputColor.extent), Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
            Vk::Slot<"blendTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(frame.graphResources.smaaWeightTarget))
        );
    });
}

} // namespace ZHLN::Passes

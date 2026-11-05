// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/SMAAPasses.hpp"
#include "Renderer.hpp"
#include "passes/FullscreenPassRecorder.hpp"
#include "passes/aa/AntiAliasingResources.hpp"
#include "passes/aa/SMAAVariants.hpp"
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

        return MakeFullscreenPassArgs(
            MetricsOf(inputColor.extent), Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
            Vk::Slot<"depthTex">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(frame.ActiveDepthTarget())),
            Vk::Slot<"normRoughTex">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(frame.graphResources.normalRoughnessBuffer))
        );
    });
}

void SmaaWeightPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const auto  extent       = frame.graphResources.smaaWeightTarget.extent;
    const auto& areaView     = frame.renderer.textureManager.View(aa.smaaAreaTexIdx);
    const auto& searchView   = frame.renderer.textureManager.View(aa.smaaSearchTexIdx);
    const auto  variantIndex = SmaaWeightVariantForExtent(extent);

    const auto block = aa.smaaWeight.WriteHeapParameters<Shaders::SmaaWeight>(
        frame.renderer.heapManager, Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(frame.graphResources.smaaEdgeTarget)),
        Vk::Slot<"areaTex">(areaView), Vk::Slot<"searchTex">(searchView)
    );

    // The three pipelines were built at init (BuildSMAAPipeline), so the
    // resolution tier only picks an index -- no shader cook or pipeline create
    // on resize. Typestate: From proves the variant's bounds and validity,
    // Create the non-zero heap push; ExecuteHeap cannot be reached otherwise.
    if (const auto active = Vk::ActivePipeline::From(aa.smaaWeight.pipelines, variantIndex)) {
        if (const auto heap = Vk::BoundHeapPass::Create(aa.smaaWeight.heapBindings.indexPushOffset, block.slot)) {
            aa.smaaWeight.ExecuteHeap<Shaders::Modules::SmaaWeightVS, Shaders::Modules::SmaaWeightPS>(ctx, *active, *heap, MetricsOf(extent));
        }
    }
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

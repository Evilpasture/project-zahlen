// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/SMAAPasses.hpp"
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
    if (impl.smaaEdgePass.pipeline.Valid()) {
        const auto& inputColor = impl.graphResources.hdrSceneColor;

        const Vk::HeapBlockBase block = impl.smaaEdgePass.WriteHeapParameters<Shaders::SmaaEdge>(
            impl.ctx, impl.heapManager, Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor))
        );
        impl.smaaEdgePass.ExecuteHeap<Shaders::Modules::SmaaEdgeVS>(impl.ctx, ctx.Cmd(), MetricsOf(inputColor.extent), block);
    }
}

void SmaaWeightPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    if (impl.smaaWeightPass.pipeline.Valid()) {
        const auto& areaView   = impl.textureManager.View(impl.postProcess.SmaaAreaTexture());
        const auto& searchView = impl.textureManager.View(impl.postProcess.SmaaSearchTexture());
        const Vk::HeapBlockBase block = impl.smaaWeightPass.WriteHeapParameters<Shaders::SmaaWeight>(
            impl.ctx, impl.heapManager,
            Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(impl.graphResources.smaaEdgeTarget)),
            Vk::Slot<"areaTex">(areaView),
            Vk::Slot<"searchTex">(searchView)
        );
        impl.smaaWeightPass.ExecuteHeap<Shaders::Modules::SmaaWeightVS, Shaders::Modules::SmaaWeightPS>(
            impl.ctx, ctx.Cmd(), MetricsOf(impl.graphResources.smaaWeightTarget.extent), block
        );
    }
}

void SmaaBlendPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    if (impl.smaaBlendPass.pipeline.Valid()) {
        const auto& inputColor = impl.graphResources.hdrSceneColor;

        const Vk::HeapBlockBase block = impl.smaaBlendPass.WriteHeapParameters<Shaders::SmaaBlend>(
            impl.ctx, impl.heapManager,
            Vk::Slot<"colorTex">(Vk::Assume<Vk::ShaderRead<Res_HdrSceneColor>>(inputColor)),
            Vk::Slot<"blendTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaWeight>>(impl.graphResources.smaaWeightTarget))
        );
        impl.smaaBlendPass.ExecuteHeap<Shaders::Modules::SmaaBlendVS, Shaders::Modules::SmaaBlendPS>(
            impl.ctx, ctx.Cmd(), MetricsOf(inputColor.extent), block
        );
    }
}

}

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/aa/SMAAPasses.hpp"
#include <ShaderBindings.hpp>
#include <tuple>

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
        const auto& [areaView, searchView] =
            std::tie(impl.textureManager.View(impl.postProcess.SmaaAreaTexture()), impl.textureManager.View(impl.postProcess.SmaaSearchTexture()));
        const auto areaInfo =
            Vk::MakeViewCreateInfo2D(impl.textureManager.Image(impl.postProcess.SmaaAreaTexture()).Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
        const auto searchInfo = Vk::MakeViewCreateInfo2D(
            impl.textureManager.Image(impl.postProcess.SmaaSearchTexture()).Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT
        );
        const auto areaHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
            .handle   = impl.textureManager.Image(impl.postProcess.SmaaAreaTexture()).Handle(),
            .view     = areaView.Get(),
            .extent   = {.width = 160, .height = 560, .depth = 1},
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .format   = VK_FORMAT_R8G8B8A8_UNORM,
            .viewInfo = &areaInfo
        };
        const auto searchHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
            .handle   = impl.textureManager.Image(impl.postProcess.SmaaSearchTexture()).Handle(),
            .view     = searchView.Get(),
            .extent   = {.width = 64, .height = 16, .depth = 1},
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .format   = VK_FORMAT_R8G8B8A8_UNORM,
            .viewInfo = &searchInfo
        };
        const Vk::HeapBlockBase block = impl.smaaWeightPass.WriteHeapParameters<Shaders::SmaaWeight>(
            impl.ctx, impl.heapManager,
            Vk::Slot<"edgesTex">(Vk::Assume<Vk::ShaderRead<Res_SmaaEdge>>(impl.graphResources.smaaEdgeTarget)),
            Vk::Slot<"areaTex">(areaHeap),
            Vk::Slot<"searchTex">(searchHeap)
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

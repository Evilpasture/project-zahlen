// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/lighting/ClusteredLightingPass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

void ClusteredLightingPass::operator()(Vk::RasterPassContextBase& ctx) const noexcept {
    const uint32_t fIdx = impl.presenter.frameIndex;

    // The pipeline has a ray-traced variant that shadows the rasterizer's
    // reflection term with a traced one; which specialization the graph runs
    // is this frame's setting, resolved here rather than being baked in.
    const bool   rtrActive    = impl.settings.rayTracing.enableReflections && impl.ctx.RayTracingSupported();
    const uint32_t lightVariant = rtrActive ? 1 : 0;

    const auto ltcMatHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
        .handle   = impl.ltcMatImage.Handle(),
        .view     = impl.ltcMatView.Get(),
        .extent   = {.width = 64, .height = 64, .depth = 1},
        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
        .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
        .viewInfo = &impl.ltcMatViewInfo
    };
    const auto ltcAmpHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
        .handle   = impl.ltcAmpImage.Handle(),
        .view     = impl.ltcAmpView.Get(),
        .extent   = {.width = 64, .height = 64, .depth = 1},
        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
        .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
        .viewInfo = &impl.ltcAmpViewInfo
    };
    const auto atlasCubeHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
        .handle   = impl.graphResources.shadowAtlas.image.Handle(),
        .view     = impl.targets.AtlasCubeView().Get(),
        .extent   = {.width = 1024, .height = 1024, .depth = 1},
        .aspect   = VK_IMAGE_ASPECT_DEPTH_BIT,
        .format   = VK_FORMAT_D32_SFLOAT,
        .viewInfo = &impl.targets.AtlasCubeViewInfo()
    };
    const auto atlas2DHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
        .handle   = impl.graphResources.shadowAtlas.image.Handle(),
        .view     = impl.targets.Atlas2DView().Get(),
        .extent   = {.width = 1024, .height = 1024, .depth = 1},
        .aspect   = VK_IMAGE_ASPECT_DEPTH_BIT,
        .format   = VK_FORMAT_D32_SFLOAT,
        .viewInfo = &impl.targets.Atlas2DViewInfo()
    };
    const auto blueNoiseHeap = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {
        .handle   = impl.textureManager.Image(impl.blueNoiseTexIdx).Handle(),
        .view     = impl.textureManager.View(impl.blueNoiseTexIdx).Get(),
        .extent   = {.width = impl.blueNoiseWidth, .height = impl.blueNoiseHeight, .depth = 1},
        .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
        .format   = VK_FORMAT_R8G8B8A8_UNORM,
        .viewInfo = &impl.blueNoiseViewInfo
    };
    const Vk::AsAddressWrite tlas {
        .address = (impl.ctx.RayTracingSupported() && impl.frames.tlas.Current()) ?
                       Vk::GetAccelerationStructureAddress(impl.ctx.Device(), impl.frames.tlas.Current().Get()) :
                       0
    };
    const Vk::HeapBlockBase block = impl.lightingPass.WriteHeapParameters<Shaders::Lighting>(
        impl.ctx, impl.heapManager,
        Vk::Slot<"texInput">(Vk::Assume<Vk::ShaderRead<Res_SceneColor>>(impl.graphResources.sceneColor)),
        Vk::Slot<"texDepth">(Vk::Assume<Vk::ShaderRead<Res_Depth>>(impl.presenter.depthTarget)),
        Vk::Slot<"texNormalRoughness">(Vk::Assume<Vk::ShaderRead<Res_NormRough>>(impl.graphResources.normalRoughnessBuffer)),
        Vk::Slot<"lights">(impl.frames.lightStorageBuffers[fIdx]),
        Vk::Slot<"frame">(impl.frames.frameUniformBuffers[fIdx]),
        Vk::Slot<"shadowMap">(Vk::Assume<Vk::ShaderRead<Res_ShadowMap>>(impl.graphResources.shadowMap)),
        Vk::Slot<"ltc_mat">(ltcMatHeap),
        Vk::Slot<"ltc_amp">(ltcAmpHeap),
        Vk::Slot<"clusterGrid">(impl.frames.clusterGridBuffers[fIdx]),
        Vk::Slot<"clusterIndexList">(impl.frames.lightIndexListBuffers[fIdx]),
        Vk::Slot<"punctualShadowCube">(atlasCubeHeap),
        Vk::Slot<"punctualShadow2D">(atlas2DHeap),
        Vk::Slot<"blueNoiseTex">(blueNoiseHeap),
        Vk::Slot<"texEmissive">(Vk::Assume<Vk::ShaderRead<Res_Emissive>>(impl.graphResources.emissiveBuffer)),
        Vk::Slot<"texAo">(Vk::Assume<Vk::ShaderRead<Res_Ao>>(impl.graphResources.ao)),
        Vk::Slot<"texClearcoat">(Vk::Assume<Vk::ShaderRead<Res_Clearcoat>>(impl.graphResources.clearcoatBuffer)),
        Vk::Slot<"tlas">(tlas)
    );
    impl.lightingPass.ExecuteVariantHeap<Shaders::Modules::LightingPS, Shaders::Modules::LightingNortPS>(impl.ctx, ctx.Cmd(), lightVariant, pc, block);
}

}

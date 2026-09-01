// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitTargets.cpp
#include "../RenderInternal.hpp"
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <algorithm>
#include <array>

namespace ZHLN {

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept {
    const auto& ctx = impl.ctx;

    Reflect::ForEachReflectedField<typename RenderContext::Impl::GraphResources::ReflectMetadata>(impl.graphResources, [&]<typename Tag>(auto& rt) {
        if constexpr (requires { rt.image.Handle(); }) {
            Vk::Debug::SetImageName(ctx, rt.image.Handle(), Tag::name.string_view());
        }
    });

    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[0].image.Handle(), "AccumHistory0");
    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[1].image.Handle(), "AccumHistory1");
    Vk::Debug::SetImageName(ctx, impl.presentation.depthTarget.image.Handle(), "DepthTarget");
    Vk::Debug::SetImageName(ctx, impl.shadowMapPrev.image.Handle(), "ShadowMapPrev");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.brdfLutImage.Handle(), "IBL.BrdfLut");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.prefilteredImage.Handle(), "IBL.PrefilteredCube");
    Vk::Debug::SetImageName(ctx, impl.ltcMatImage.Handle(), "LTC.Mat");
    Vk::Debug::SetImageName(ctx, impl.ltcAmpImage.Handle(), "LTC.Amp");

    for (size_t i = 0; i < impl.textureImages.size(); ++i) {
        Vk::Debug::SetImageName(ctx, impl.textureImages[i].Handle(), std::format("BindlessTexture{:03}", i));
    }

    const auto& swapchain = impl.presentation.swapchain.Get();
    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        Vk::Debug::SetImageName(ctx, swapchain.images[i], std::format("Swapchain{}", i));
    }
}

void RenderContext::Impl::RecreatePunctualShadowViews() noexcept {
    punctualShadowViews.clear();
    punctualShadowViews.resize(MAX_PUNCTUAL_LIGHTS);
    for (uint32_t i = 0; i < MAX_PUNCTUAL_LIGHTS; ++i) {
        auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
            ctx.Device(), graphResources.shadowAtlas.image.Handle(),
            i * 6,                    // baseLayer
            6,                        // layerCount
            VK_IMAGE_ASPECT_DEPTH_BIT // aspect
        );
        if (view_res.has_value()) {
            punctualShadowViews[i] = std::move(*view_res);
        }
    }
}

std::expected<void, Error> RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
    if (!presentation.Rebuild(ext.width, ext.height)) {
        return std::unexpected(Vk::PresentationError::SwapchainCreationFailed);
    }

    const auto& voxelDispatch = volumetricClearPass.fixedDispatchSize;
    if (voxelDispatch[0] == 0 || voxelDispatch[1] == 0 || voxelDispatch[2] == 0) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    const VkExtent3D voxelExt = {.width = voxelDispatch[0], .height = voxelDispatch[1], .depth = voxelDispatch[2]};

    auto assign = [&](auto& member, auto e) -> std::expected<void, Error> {
        if (!e) {
            return std::unexpected(e.error());
        }
        member = std::move(*e);
        return {};
    };

    std::expected<void, Error> result {};

    result = assign(frames.accumBuffers[0], CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    if (result) {
        result = assign(frames.accumBuffers[1], CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext, VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    }

    // Standard 2D (plus scale_divisor), 3D voxels, TransDepth, and HiZ are
    // driven by ReflectMetadata. Shadow atlas/map stay in InitShadowResources.
    Reflect::ForEachReflectedField<GraphResources::ReflectMetadata>(graphResources, [&]<typename Tag>(auto& rt) {
        if (!result) {
            return;
        }
        // else-if so CreateDefaultTarget is discarded for 3D / Hi-Z / depth /
        // atlas tags (a plain `return` after if constexpr still instantiates
        // the 2D path for every Tag).
        if constexpr (Tag::is_swapchain || std::is_same_v<Tag, Res_ShadowAtlas> || std::is_same_v<Tag, Res_ShadowMap>) {
            return;
        } else if constexpr (Tag::is_3d) {
            result = assign(
                rt, Vk::RenderTarget3D<Tag::format>::Create(
                        allocator, ctx, voxelExt, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    )
            );
        } else if constexpr (requires {
                                 rt.mipLevels;
                                 rt.mipViews;
                             }) {
            result = assign(
                rt, Vk::MipmappedRenderTarget<Tag::format>::Create(
                        allocator, ctx, ext,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    )
            );
        } else if constexpr ((Tag::aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            result = assign(
                rt,
                Vk::RenderTarget<Tag::format>::Create(allocator, ctx, ext, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT})
            );
        } else {
            VkImageUsageFlags extra = 0;
            if constexpr (std::is_same_v<Tag, Res_HdrSceneColor>) {
                extra = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            // The Dual Kawase bloom chain writes every cascade level with
            // compute imageStores, so all downscaled bloom targets need
            // storage-image usage on top of the usual attachment/sampled bits.
            if constexpr (Tag::scale_divisor > 1) {
                extra |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            // The A-Trous HDR denoiser stores through a UAV: the two
            // ping-pong scratch targets plus the final write-back into
            // hdrSceneColor must all carry storage-image usage.
            if constexpr (std::is_same_v<Tag, Res_HdrSceneColor> || std::is_same_v<Tag, Res_DenoiseA> || std::is_same_v<Tag, Res_DenoiseB>) {
                extra |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            const VkExtent2D scaled = {.width = std::max(1u, ext.width / Tag::scale_divisor), .height = std::max(1u, ext.height / Tag::scale_divisor)};
            result                  = assign(rt, CreateDefaultTarget<Tag::format>(scaled, extra));
        }
    });

    if (!result) {
        return result;
    }

    RecreatePunctualShadowViews();

    // Transition all newly allocated render targets to their correct default layouts
    Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
        // History-bearing targets are READ before their first full-coverage
        // write: TAA samples AccumCurr on frame 0, and the volumetric
        // temporal filter samples VoxelHist before it ever wrote it (and the
        // graphics queue reads VoxelResolved one compute-submission early).
        // Leaving the content as VRAM garbage made the very first frames
        // differ between runs — worse, NaN bit patterns survive the
        // neighborhood clamps and poison temporal accumulation indefinitely.
        // Clear every target whose first definition is a read.
        const VkClearColorValue       clearBlack = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
        const VkImageSubresourceRange clearRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS
        };
        const std::array accumImages = {frames.accumBuffers[0].image.Handle(), frames.accumBuffers[1].image.Handle()};
        for (const auto img: accumImages) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        const std::array targets3D = {
            graphResources.voxelMedia.image.Handle(), graphResources.voxelLight.image.Handle(), graphResources.voxelIntegrated.image.Handle(),
            graphResources.voxelHistory.image.Handle(), graphResources.voxelResolved.image.Handle()
        };
        for (auto* const img: targets3D) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        std::array colorTargets = {graphResources.sceneColor.image.Handle(),
                                   graphResources.velocityBuffer.image.Handle(),
                                   graphResources.normalRoughnessBuffer.image.Handle(),
                                   graphResources.emissiveBuffer.image.Handle(),
                                   graphResources.hdrSceneColor.image.Handle(),
                                   graphResources.lightingTarget.image.Handle(),
                                   graphResources.smaaEdgeTarget.image.Handle(),
                                   graphResources.smaaWeightTarget.image.Handle(),
                                   graphResources.transNormalBuffer.image.Handle(),
                                   graphResources.transLightingTarget.image.Handle()};

        for (auto* const img: colorTargets) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        // The Kawase bloom chain is pure compute now: every level is written
        // with imageStore and re-read as a sampled image inside one graph pass,
        // all in GENERAL layout. Park the targets in their steady-state layout
        // right after allocation (the graph still transitions them from
        // UNDEFINED on the first use of every frame).
        const std::array bloomComputeTargets = {graphResources.bloomThresholdTarget.image.Handle(),
                                                graphResources.bloomDown1.image.Handle(),
                                                graphResources.bloomDown2.image.Handle(),
                                                graphResources.bloomDown3.image.Handle(),
                                                graphResources.bloomUp2.image.Handle(),
                                                graphResources.bloomUp1.image.Handle(),
                                                graphResources.bloomFinalTarget.image.Handle()};
        for (auto* const img: bloomComputeTargets) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        // The HDR A-Trous denoiser ping-pongs through the same GENERAL-layout
        // compute-only pattern.
        const std::array denoiseTargets = {graphResources.denoiseA.image.Handle(), graphResources.denoiseB.image.Handle()};
        for (auto* const img: denoiseTargets) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        // VK_EXT_descriptor_heap: the decal pass samples the depth target
        // through the heap, so rewrite its descriptor whenever the target is
        // recreated (the old view was destroyed). Depth/stencil sampled-image
        // descriptors must select exactly one aspect (VUID-VkImageDescriptorInfoEXT-pView-11430);
        // decal.slang only reads the depth value.
        if (decalDepthSlot.Valid()) {
            const auto info = Vk::MakeViewCreateInfo2D(presentation.depthTarget.image.Handle(), VK_FORMAT_D32_SFLOAT_S8_UINT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
            heapManager.WriteImage(decalDepthSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
            cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
            cmd, graphResources.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, graphResources.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );

        const VkClearColorValue clearFarDepth = {.float32 = {1.0F, 1.0F, 1.0F, 1.0F}};
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
            cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT
        );
        vkCmdClearColorImage(cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearFarDepth, 1, &clearRange);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, graphResources.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT
        );
    });

    WriteTransLightingToHeap();

    // VK_EXT_descriptor_heap: rewrite the Hi-Z descriptor slots.
    const uint32_t mips = std::min<uint32_t>(graphResources.hizMap.mipLevels, 16);
    for (uint32_t m = 0; m < mips; ++m) {
        const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> outMip {
            .handle   = graphResources.hizMap.image.Handle(),
            .view     = graphResources.hizMap.mipViews[m].Get(),
            .extent   = {.width = graphResources.hizMap.extent.width, .height = graphResources.hizMap.extent.height, .depth = 1},
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .format   = VK_FORMAT_R32_SFLOAT,
            .viewInfo = &graphResources.hizMap.mipViewInfos[m]
        };
        if (m == 0) {
            heapManager.WriteBindings(ctx, hizHeapBindings, m, Vk::Assume<Vk::ComputeRead<Res_Depth>>(presentation.depthTarget), outMip, Vk::SkipWrite {});
        } else {
            const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> inMip {
                .handle   = graphResources.hizMap.image.Handle(),
                .view     = graphResources.hizMap.mipViews[m - 1].Get(),
                .extent   = {.width = graphResources.hizMap.extent.width, .height = graphResources.hizMap.extent.height, .depth = 1},
                .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
                .format   = VK_FORMAT_R32_SFLOAT,
                .viewInfo = &graphResources.hizMap.mipViewInfos[m - 1]
            };
            heapManager.WriteBindings(ctx, hizHeapBindings, m, inMip, outMip, Vk::SkipWrite {});
        }
    }

    for (uint32_t idx = 0; idx < 4; ++idx) {
        const uint32_t pass     = idx >> 1;
        const uint32_t parity   = idx & 1;
        const auto&    indirect = (pass == 0) ? frames.indirectCommandsBuffers[parity] : frames.indirectCommandsBuffersPass2[parity];
        heapManager.WriteBindings(
            ctx, cullingHeapBindings, idx, frames.instanceDataBuffers[parity], indirect, Vk::Assume<Vk::ComputeRead<Res_HiZ>>(graphResources.hizMap),
            Vk::SkipWrite {}, frames.secondPassCandidatesBuffers[parity], frames.secondPassCountBuffers[parity]
        );
    }

    ApplyImageDebugNames(*this);
    return {};
}

} // namespace ZHLN

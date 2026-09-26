// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/TargetManager.cpp

#include "TargetManager.hpp"

#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>

namespace ZHLN {

auto TargetManager::CreateCascadeViews(VkImage image, ZHLN::Array<Vk::ImageView>& out) const -> std::expected<void, ErrorCode> {
    out.clear();
    out.resize(kCascades);
    for (uint32_t i = 0; i < kCascades; ++i) {
        auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(_ctx.Device(), image, i, 1);
        if (!view_res) {
            return std::unexpected(view_res.error());
        }
        out[i] = std::move(*view_res);
    }
    return {};
}

auto TargetManager::Recreate(VkExtent2D ext, VkExtent3D voxelExtent) -> std::expected<void, ErrorCode> {
    auto assign = [&](auto& member, auto e) -> std::expected<void, ErrorCode> {
        if (!e) {
            return std::unexpected(e.error());
        }
        member = std::move(*e);
        return {};
    };

    std::expected<void, ErrorCode> result {};

    // Standard 2D (plus scale_divisor), 3D voxels, TransDepth and Hi-Z are all
    // driven by the reflected metadata, so a new target is created and bound by
    // adding it to GraphResources and its metadata -- nothing here changes.
    // The cascade shadow map and the punctual atlas are skipped: they are sized
    // by a graphics setting rather than the window, so InitShadows and
    // ResizeShadows own them.
    Reflect::ForEachReflectedField<GraphResources::ReflectMetadata>(_graph, [&]<typename Tag>(auto& rt) {
        if (!result) {
            return;
        }
        // else-if so CreateColorTarget is discarded for 3D / Hi-Z / depth /
        // atlas tags (a plain `return` after if constexpr still instantiates
        // the 2D path for every Tag).
        if constexpr (Tag::is_swapchain || std::is_same_v<Tag, Res_ShadowAtlas> || std::is_same_v<Tag, Res_ShadowMap>) {
            return;
        } else if constexpr (Tag::is_3d) {
            result = assign(
                rt, Vk::RenderTarget3D<Tag::format>::Create(
                        _allocator, _ctx, voxelExtent, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled | Vk::ImageUsage::TransferDst
                    )
            );
        } else if constexpr (requires {
                                 rt.mipLevels;
                                 rt.mipViews;
                             }) {
            result = assign(
                rt, Vk::MipmappedRenderTarget<Tag::format>::Create(
                        _allocator, _ctx, ext,
                        Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | Vk::ImageUsage::Storage | Vk::ImageUsage::TransferSrc |
                            Vk::ImageUsage::TransferDst
                    )
            );
        } else if constexpr ((Tag::aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
            result = assign(
                rt,
                Vk::RenderTarget<Tag::format>::Create(_allocator, _ctx, ext, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled})
            );
        } else {
            Vk::ImageUsage extra = Vk::ImageUsage::None;
            if constexpr (std::is_same_v<Tag, Res_HdrSceneColor>) {
                extra = Vk::ImageUsage::TransferSrc;
            }
            // Transmission copies the lit scene into this target before the
            // forward pass samples it. The copy is a transfer, not a draw.
            if constexpr (std::is_same_v<Tag, Res_TransLighting>) {
                extra = Vk::ImageUsage::TransferDst;
            }
            // The Dual Kawase bloom chain writes every cascade level with
            // compute imageStores, so all downscaled bloom targets need
            // storage-image usage on top of the usual attachment/sampled bits.
            if constexpr (Tag::scale_divisor > 1) {
                extra |= Vk::ImageUsage::Storage;
            }
            // The A-Trous HDR denoiser stores through a UAV: the two
            // ping-pong scratch targets plus the final write-back into
            // hdrSceneColor must all carry storage-image usage.
            if constexpr (std::is_same_v<Tag, Res_HdrSceneColor> || std::is_same_v<Tag, Res_DenoiseA> || std::is_same_v<Tag, Res_DenoiseB>) {
                extra |= Vk::ImageUsage::Storage;
            }
            const VkExtent2D scaled = {.width = std::max(1u, ext.width / Tag::scale_divisor), .height = std::max(1u, ext.height / Tag::scale_divisor)};
            result                  = assign(rt, CreateColorTarget<Tag::format>(_allocator, _ctx, scaled, extra));
        }
    });

    if (!result) {
        return result;
    }

    RecreatePunctualShadowViews();
    return {};
}

auto TargetManager::InitShadows() -> std::expected<void, ErrorCode> {
    const VkExtent2D cascadeExt = {.width = kShadowResolution, .height = kShadowResolution};
    const VkExtent2D atlasExt   = {.width = kAtlasSize, .height = kAtlasSize};

    auto sm_res =
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(_allocator, _ctx, cascadeExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades});
    if (!sm_res) {
        return std::unexpected(sm_res.error());
    }
    _graph.shadowMap = std::move(*sm_res);

    auto smp_res =
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(_allocator, _ctx, cascadeExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades});
    if (!smp_res) {
        return std::unexpected(smp_res.error());
    }
    _shadowMapPrev = std::move(*smp_res);

    if (auto r = CreateCascadeViews(_graph.shadowMap.image.Handle(), _shadowCascadeViews); !r) {
        return r;
    }
    if (auto r = CreateCascadeViews(_shadowMapPrev.image.Handle(), _shadowCascadeViewsPrev); !r) {
        return r;
    }
    for (uint32_t i = 0; i < kCascades; ++i) {
        if (!_shadowCascadeViews[i].Valid() || !_shadowCascadeViewsPrev[i].Valid()) [[unlikely]] {
            return std::unexpected(Vk::ImageViewCreationError::CreationFailed);
        }
    }

    // The punctual atlas holds kPunctualLights lights x 6 cube faces.
    auto sa_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
        _allocator, _ctx, atlasExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kAtlasLayers}
    );
    if (!sa_res) [[unlikely]] {
        return std::unexpected(sa_res.error());
    }
    _graph.shadowAtlas = std::move(*sa_res);

    // Bound two ways: as a cube array the shadow pass indexes per light, and as
    // a flat 2D array for anything that walks every layer at once.
    const VkImage atlas = _graph.shadowAtlas.image.Handle();
    _shadowAtlasCubeViewInfo = Vk::MakeViewCreateInfoCubeArray(atlas, VK_FORMAT_D32_SFLOAT, kAtlasLayers, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    _shadowAtlas2DViewInfo   = Vk::MakeViewCreateInfo2DArray(atlas, VK_FORMAT_D32_SFLOAT, 0, kAtlasLayers, VK_IMAGE_ASPECT_DEPTH_BIT, 1);

    auto cube_res = Vk::CreateView(_ctx.Device(), _shadowAtlasCubeViewInfo);
    if (!cube_res) {
        return std::unexpected(cube_res.error());
    }
    _shadowAtlasCubeView = std::move(*cube_res);

    auto array_res = Vk::CreateView(_ctx.Device(), _shadowAtlas2DViewInfo);
    if (!array_res) {
        return std::unexpected(array_res.error());
    }
    _shadowAtlas2DView = std::move(*array_res);

    if (!_shadowAtlasCubeView.Valid() || !_shadowAtlas2DView.Valid()) [[unlikely]] {
        return std::unexpected(Vk::ImageViewCreationError::CreationFailed);
    }

    // Pure layout transitions, so this goes straight to the queue rather than
    // through the staging ring's timeline: nothing here writes staged data, and
    // ResizeShadows below records the same transitions the same way.
    Vk::ExecuteImmediate(_ctx, _ring, [&](VkCommandBuffer cmd) -> void {
        const std::array shadows = {_graph.shadowMap.image.Handle(), _shadowMapPrev.image.Handle(), _graph.shadowAtlas.image.Handle()};
        for (const auto img: shadows) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
    });

    RecreatePunctualShadowViews();
    return {};
}

auto TargetManager::ResizeShadows(uint32_t resolution) noexcept -> std::expected<void, ErrorCode> {
    auto* device = _ctx.Device();

    return Vk::WaitIdle(device)
        .transform_error([](auto) -> ErrorCode { return ShadowResolutionError::RecreationFailed; })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            const VkExtent2D ext = {.width = resolution, .height = resolution};

            auto sm_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                _allocator, _ctx, ext, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
            );
            if (!sm_res) {
                return std::unexpected(sm_res.error());
            }
            _graph.shadowMap = std::move(*sm_res);

            auto smp_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                _allocator, _ctx, ext, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
            );
            if (!smp_res) {
                return std::unexpected(smp_res.error());
            }
            _shadowMapPrev = std::move(*smp_res);

            if (auto r = CreateCascadeViews(_graph.shadowMap.image.Handle(), _shadowCascadeViews); !r) {
                return r;
            }
            if (auto r = CreateCascadeViews(_shadowMapPrev.image.Handle(), _shadowCascadeViewsPrev); !r) {
                return r;
            }

            Vk::ExecuteImmediate(_ctx, _ring, [&](VkCommandBuffer cmd) -> void {
                const std::array shadows = {_graph.shadowMap.image.Handle(), _shadowMapPrev.image.Handle()};
                for (const auto img: shadows) {
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
                }
            });

            ZHLN::Log("Shadow map dynamically resized on the GPU to {}x{}", resolution, resolution);
            return {};
        });
}

void TargetManager::RecreatePunctualShadowViews() noexcept {
    _punctualShadowViews.clear();
    _punctualShadowViews.resize(kPunctualLights);
    for (uint32_t i = 0; i < kPunctualLights; ++i) {
        auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
            _ctx.Device(), _graph.shadowAtlas.image.Handle(),
            i * 6,                    // baseLayer
            6,                        // layerCount
            VK_IMAGE_ASPECT_DEPTH_BIT // aspect
        );
        if (view_res.has_value()) {
            _punctualShadowViews[i] = std::move(*view_res);
        }
    }
}

void TargetManager::RecordInitialLayouts(VkCommandBuffer cmd) const noexcept {
    // History-bearing targets are READ before their first full-coverage write:
    // TAA samples AccumCurr on frame 0, and the volumetric temporal filter
    // samples VoxelHist before it ever wrote it (and the graphics queue reads
    // VoxelResolved one compute-submission early). Leaving the content as VRAM
    // garbage made the very first frames differ between runs -- worse, NaN bit
    // patterns survive the neighborhood clamps and poison temporal accumulation
    // indefinitely. Clear every target whose first definition is a read.
    const VkClearColorValue       clearBlack = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
    const VkImageSubresourceRange clearRange = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS
    };

    const std::array targets3D = {_graph.voxelMedia.image.Handle(),   _graph.voxelLight.image.Handle(),      _graph.voxelIntegrated.image.Handle(),
                                  _graph.voxelHistory.image.Handle(), _graph.voxelResolved.image.Handle()};
    for (auto* const img: targets3D) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const std::array colorTargets = {_graph.sceneColor.image.Handle(),
                                     _graph.velocityBuffer.image.Handle(),
                                     _graph.normalRoughnessBuffer.image.Handle(),
                                     _graph.emissiveBuffer.image.Handle(),
                                     _graph.hdrSceneColor.image.Handle(),
                                     _graph.lightingTarget.image.Handle(),
                                     _graph.smaaEdgeTarget.image.Handle(),
                                     _graph.smaaWeightTarget.image.Handle(),
                                     _graph.transNormalBuffer.image.Handle(),
                                     _graph.transLightingTarget.image.Handle()};
    for (auto* const img: colorTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // The Kawase bloom chain is pure compute now: every level is written with
    // imageStore and re-read as a sampled image inside one graph pass, all in
    // GENERAL layout. Park the targets in their steady-state layout right after
    // allocation (the graph still transitions them from UNDEFINED on the first
    // use of every frame).
    const std::array bloomComputeTargets = {_graph.bloomThresholdTarget.image.Handle(), _graph.bloomDown1.image.Handle(), _graph.bloomDown2.image.Handle(),
                                            _graph.bloomDown3.image.Handle(),           _graph.bloomUp2.image.Handle(),   _graph.bloomUp1.image.Handle(),
                                            _graph.bloomFinalTarget.image.Handle()};
    for (auto* const img: bloomComputeTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // The HDR A-Trous denoiser ping-pongs through the same GENERAL-layout
    // compute-only pattern.
    const std::array denoiseTargets = {_graph.denoiseA.image.Handle(), _graph.denoiseB.image.Handle()};
    for (auto* const img: denoiseTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Transparency carries its own depth/stencil target.
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
        cmd, _graph.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
    );
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        cmd, _graph.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
    );

    // Hi-Z starts at far depth so the first frame's occlusion tests reject
    // nothing rather than everything.
    const VkClearColorValue clearFarDepth = {.float32 = {1.0F, 1.0F, 1.0F, 1.0F}};
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, _graph.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdClearColorImage(cmd, _graph.hizMap.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearFarDepth, 1, &clearRange);
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, _graph.hizMap.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT);
}

void TargetManager::NameGraphTargets() const noexcept {
    Reflect::ForEachReflectedField<GraphResources::ReflectMetadata>(_graph, [&]<typename Tag>(auto& rt) {
        if constexpr (requires { rt.image.Handle(); }) {
            Vk::Debug::SetImageName(_ctx, rt.image.Handle(), Tag::name.string_view());
        }
    });
}

} // namespace ZHLN

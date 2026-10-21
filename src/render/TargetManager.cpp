// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TargetManager.hpp"
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>

namespace ZHLN {

auto TargetManager::CreateCascadeViews(VkDevice device, const Vk::Image& image) const -> std::expected<ZHLN::Array<Vk::AttachmentView>, ErrorCode> {
    ZHLN::Array<Vk::AttachmentView> out;
    out.resize(kCascades);
    for (uint32_t i = 0; i < kCascades; ++i) {
        auto view_res = image.CreateAttachmentView(device, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        if (!view_res) {
            return std::unexpected(view_res.error());
        }
        out[i] = std::move(*view_res);
    }
    return out;
}

auto TargetManager::Recreate(VkExtent2D ext, VkExtent3D voxelExtent) -> std::expected<void, ErrorCode> {
    std::expected<void, ErrorCode> result {};
    Reflect::ForEachField(_graph, [&]<typename TargetT>(TargetT& rt) {
        if (!result)
            return;
        using Resource = typename TargetT::TagType;
        if constexpr (Resource::is_swapchain || std::is_same_v<Resource, Res_ShadowAtlas> || std::is_same_v<Resource, Res_ShadowMap>) {
            return; // The swapchain and shadow dimensions have independent owners.
        } else {
            VkExtent2D     requested = ext;
            Vk::ImageUsage extra     = Vk::ImageUsage::None;
            // DDGI atlases have fixed shader-addressed resolutions, not a
            // fraction of the viewport; preserve their old allocation policy.
            if constexpr (std::is_same_v<Resource, Res_DdgiIrradiance>) {
                requested = {640u, 320u};
                extra |= Vk::ImageUsage::Storage | Vk::ImageUsage::TransferDst;
            } else if constexpr (std::is_same_v<Resource, Res_DdgiDistance>) {
                requested = {1024u, 512u};
                extra |= Vk::ImageUsage::Storage | Vk::ImageUsage::TransferDst;
            } else if constexpr (std::is_same_v<Resource, Res_DdgiRays>) {
                requested = {2048u, 64u};
                extra |= Vk::ImageUsage::Storage | Vk::ImageUsage::TransferDst;
            }
            // OpaqueSceneCopyPass transitions this image to TRANSFER_SRC and
            // copies it into TransLighting. Storage and TransferDst alone are
            // insufficient even though both allow other GPU write paths.
            if constexpr (std::is_same_v<Resource, Res_HdrSceneColor>) {
                extra |= Vk::ImageUsage::TransferSrc;
            }
            if constexpr (
                std::is_same_v<Resource, Res_HdrSceneColor> || std::is_same_v<Resource, Res_DenoiseA> || std::is_same_v<Resource, Res_DenoiseB> ||
                std::is_same_v<Resource, Res_RtShadowMask> || std::is_same_v<Resource, Res_RtShadowHistory> || std::is_same_v<Resource, Res_ShadowDenoiseA> ||
                std::is_same_v<Resource, Res_ShadowDenoiseB>
            ) {
                extra |= Vk::ImageUsage::Storage | Vk::ImageUsage::TransferDst;
            }
            if constexpr (
                std::is_same_v<Resource, Res_RtrHalf> || std::is_same_v<Resource, Res_RtrHistory> || std::is_same_v<Resource, Res_RtgiHalf> ||
                std::is_same_v<Resource, Res_RtgiHistory>
            ) {
                extra |= Vk::ImageUsage::TransferDst;
            }
            if constexpr (std::is_same_v<Resource, Res_LightingGI> || std::is_same_v<Resource, Res_HiZ>) {
                extra |= Vk::ImageUsage::Storage;
            }
            auto created = TargetT::Create(_allocator, _ctx.Device(), requested, voxelExtent, extra);
            if (!created) {
                result = std::unexpected(created.error());
                return;
            }
            rt.Destroy(_allocator);
            rt = std::move(*created);
        }
    });
    return result;
}

auto TargetManager::InitShadows() -> std::expected<void, ErrorCode> {
    const VkExtent2D cascadeExt = {.width = kShadowResolution, .height = kShadowResolution};
    const VkExtent2D atlasExt   = {.width = kShadowAtlasSize, .height = kShadowAtlasSize};

    auto sm_res = Vk::RenderTarget<Res_ShadowMap>::Create(
        _allocator, _ctx.Device(), cascadeExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
    );
    if (!sm_res) {
        return std::unexpected(sm_res.error());
    }

    _shadowCascadeViews.clear();
    _graph.shadowMap.Destroy(_allocator);
    _graph.shadowMap = std::move(*sm_res);

    auto smp_res = Vk::RenderTarget<Res_ShadowMap>::Create(
        _allocator, _ctx.Device(), cascadeExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
    );
    if (!smp_res) {
        return std::unexpected(smp_res.error());
    }
    _shadowCascadeViewsPrev.clear();
    _shadowMapPrev.Destroy(_allocator);
    _shadowMapPrev = std::move(*smp_res);

    if (auto r = CreateCascadeViews(_ctx.Device(), _graph.shadowMap.image); !r) {
        return std::unexpected(r.error());
    } else {
        _shadowCascadeViews = std::move(*r);
    }
    if (auto r = CreateCascadeViews(_ctx.Device(), _shadowMapPrev.image); !r) {
        return std::unexpected(r.error());
    } else {
        _shadowCascadeViewsPrev = std::move(*r);
    }
    for (uint32_t i = 0; i < kCascades; ++i) {
        if (!_shadowCascadeViews[i].Valid() || !_shadowCascadeViewsPrev[i].Valid()) [[unlikely]] {
            return std::unexpected(Vk::AttachmentViewCreationError::CreationFailed);
        }
    }

    auto sa_res = Vk::RenderTarget<Res_ShadowAtlas>::Create(
        _allocator, _ctx.Device(), atlasExt, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled}
    );
    if (!sa_res) [[unlikely]] {
        return std::unexpected(sa_res.error());
    }
    _shadowAtlasView = {};
    _graph.shadowAtlas.Destroy(_allocator);
    _graph.shadowAtlas = std::move(*sa_res);

    auto atlas_res = _graph.shadowAtlas.image.CreateAttachmentView(_ctx.Device(), VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!atlas_res) {
        return std::unexpected(atlas_res.error());
    }
    _shadowAtlasView = std::move(*atlas_res);

    if (!_shadowAtlasView.Valid()) [[unlikely]] {
        return std::unexpected(Vk::AttachmentViewCreationError::CreationFailed);
    }

    if (auto immediate = Vk::ExecuteImmediate(
            _ctx, _ring,
            [&](VkCommandBuffer cmd) -> void {
                const std::array shadows = {_graph.shadowMap.image.Handle(), _shadowMapPrev.image.Handle(), _graph.shadowAtlas.image.Handle()};
                for (const auto img: shadows) {
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
                    Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                        cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT
                    );
                }
            }
        );
        !immediate) [[unlikely]] {
        return std::unexpected(immediate.error());
    }

    return {};
}

auto TargetManager::ResizeShadows(uint32_t resolution) noexcept -> std::expected<void, ErrorCode> {
    auto* device = _ctx.Device();

    return Vk::WaitIdle(device).transform_error(
                                   [](auto) -> ErrorCode { return ShadowResolutionError::RecreationFailed; }
    ).and_then([&]() -> std::expected<void, ErrorCode> {
        const VkExtent2D ext = {.width = resolution, .height = resolution};

        auto sm_res = Vk::RenderTarget<Res_ShadowMap>::Create(
            _allocator, _ctx.Device(), ext, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
        );
        if (!sm_res) {
            return std::unexpected(sm_res.error());
        }
        auto smp_res = Vk::RenderTarget<Res_ShadowMap>::Create(
            _allocator, _ctx.Device(), ext, {.usage = Vk::ImageUsage::DepthStencilAttachment | Vk::ImageUsage::Sampled, .arrayLayers = kCascades}
        );
        if (!smp_res) {
            sm_res->Destroy(_allocator);
            return std::unexpected(smp_res.error());
        }

        _shadowCascadeViews.clear();
        _shadowCascadeViewsPrev.clear();
        _graph.shadowMap.Destroy(_allocator);
        _shadowMapPrev.Destroy(_allocator);
        _graph.shadowMap = std::move(*sm_res);
        _shadowMapPrev   = std::move(*smp_res);

        if (auto r = CreateCascadeViews(_ctx.Device(), _graph.shadowMap.image); !r) {
            return std::unexpected(r.error());
        } else {
            _shadowCascadeViews = std::move(*r);
        }
        if (auto r = CreateCascadeViews(_ctx.Device(), _shadowMapPrev.image); !r) {
            return std::unexpected(r.error());
        } else {
            _shadowCascadeViewsPrev = std::move(*r);
        }

        if (auto immediate = Vk::ExecuteImmediate(
                _ctx, _ring,
                [&](VkCommandBuffer cmd) -> void {
                    const std::array shadows = {_graph.shadowMap.image.Handle(), _shadowMapPrev.image.Handle()};
                    for (const auto img: shadows) {
                        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT);
                        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                            cmd, img, VK_IMAGE_ASPECT_DEPTH_BIT
                        );
                    }
                }
            );
            !immediate) [[unlikely]] {
            return std::unexpected(immediate.error());
        }

        ZHLN::Log("Shadow map dynamically resized on the GPU to {}x{}", resolution, resolution);
        return {};
    });
}

void TargetManager::RecordInitialLayouts(VkCommandBuffer cmd) const noexcept {
    Vk::ClearColorAndTransition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(
        cmd, Color4 {0.0f, 0.0f, 0.0f, 0.0f}, _graph.voxelMedia, _graph.voxelLight, _graph.voxelIntegrated, _graph.voxelHistory, _graph.voxelResolved
    );

    const std::array colorTargets = {
        _graph.sceneColor.image.Handle(),
        _graph.velocityBuffer.image.Handle(),
        _graph.normalRoughnessBuffer.image.Handle(),
        _graph.bakedMask.image.Handle(),
        _graph.hdrSceneColor.image.Handle(),
        _graph.lightingTarget.image.Handle(),
        _graph.lightingAo.image.Handle(),
        _graph.smaaEdgeTarget.image.Handle(),
        _graph.smaaWeightTarget.image.Handle(),
        _graph.transNormalBuffer.image.Handle(),
        _graph.transAnisotropyBuffer.image.Handle(),
        _graph.transSheenBuffer.image.Handle(),
        _graph.transLightingTarget.image.Handle()
    };
    for (auto* const img: colorTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const std::array bloomComputeTargets = {_graph.bloomThresholdTarget.image.Handle(), _graph.bloomDown1.image.Handle(), _graph.bloomDown2.image.Handle(),
                                            _graph.bloomDown3.image.Handle(),           _graph.bloomUp2.image.Handle(),   _graph.bloomUp1.image.Handle(),
                                            _graph.bloomFinalTarget.image.Handle()};
    for (auto* const img: bloomComputeTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const std::array denoiseTargets = {_graph.denoiseA.image.Handle(),       _graph.denoiseB.image.Handle(),     _graph.shadowDenoiseA.image.Handle(),
                                       _graph.shadowDenoiseB.image.Handle(), _graph.rtgiDenoiseA.image.Handle(), _graph.rtgiDenoiseB.image.Handle(),
                                       _graph.ddgiRays.image.Handle()};
    for (auto* const img: denoiseTargets) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    Vk::ClearColorAndTransition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        cmd, Color4 {1.0f, 1.0f, 1.0f, 1.0f}, _graph.rtShadowMask, _graph.rtShadowHistory
    );
    Vk::ClearColorAndTransition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        cmd, Color4 {0.0f, 0.0f, 0.0f, 0.0f}, _graph.rtrHalf, _graph.rtrHistory, _graph.rtgiHalf, _graph.rtgiHistory, _graph.ddgiIrradiance
    );
    Vk::ClearColorAndTransition<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        cmd, _graph.ddgiDistance, Color4 {50.0f, 2500.0f, 0.0f, 0.0f}
    );

    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
        cmd, _graph.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
    );
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        cmd, _graph.transDepthBuffer.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
    );

    Vk::ClearColorAndTransition(cmd, _graph.hizMap, 1.0f);
}

void TargetManager::Clear() noexcept {
    _shadowCascadeViews.clear();
    _shadowCascadeViewsPrev.clear();
    _shadowAtlasView = {};
    Reflect::ForEachField(_graph, [&](auto& target) { target.Destroy(_allocator); });
    _shadowMapPrev.Destroy(_allocator);
}

void TargetManager::NameGraphTargets() const noexcept {
    Reflect::ForEachField(_graph, [&](const auto& rt) {
        using Resource = typename std::remove_cvref_t<decltype(rt)>::TagType;
        if (rt.Valid())
            Vk::Debug::SetImageName(_ctx, rt.image.Handle(), Resource::name.string_view());
    });
}

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "Rendering.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

enum class ShadowResolutionError : uint8_t {
    DeviceLost       ZHLN_ANNOTATION(ZHLN::Description<"Device lost while resizing shadow map"> {}) = 1,
    RecreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Shadow map recreation failed"> {}),
};

class TargetManager {
  public:
    struct GraphResources {
        Vk::RenderTarget<Res_SceneColor> sceneColor;
        Vk::RenderTarget<Res_Velocity> velocityBuffer;
        Vk::RenderTarget<Res_NormRough> normalRoughnessBuffer;
        Vk::RenderTarget<Res_BakedMask> bakedMask;
        Vk::RenderTarget<Res_Lighting> lightingTarget;
        Vk::RenderTarget<Res_HdrSceneColor> hdrSceneColor;
        Vk::RenderTarget<Res_RcgiProbes> rcgiProbes;
        Vk::RenderTarget<Res_RcgiCascadeA> rcgiCascadeA;
        Vk::RenderTarget<Res_RcgiCascadeB> rcgiCascadeB;
        Vk::RenderTarget<Res_LightingGI> lightingGI;
        Vk::RenderTarget<Res_LightingAo> lightingAo;
        Vk::RenderTarget<Res_Ao> ao;
        Vk::RenderTarget<Res_BloomThresh> bloomThresholdTarget;
        Vk::RenderTarget<Res_BloomDown1> bloomDown1;
        Vk::RenderTarget<Res_BloomDown2> bloomDown2;
        Vk::RenderTarget<Res_BloomDown3> bloomDown3;
        Vk::RenderTarget<Res_BloomUp2> bloomUp2;
        Vk::RenderTarget<Res_BloomUp1> bloomUp1;
        Vk::RenderTarget<Res_BloomFinal> bloomFinalTarget;
        Vk::RenderTarget<Res_SmaaEdge> smaaEdgeTarget;
        Vk::RenderTarget<Res_SmaaWeight> smaaWeightTarget;
        Vk::RenderTarget<Res_ShadowMap> shadowMap;
        Vk::RenderTarget<Res_ShadowAtlas> shadowAtlas;
        Vk::RenderTarget<Res_VoxelMedia> voxelMedia;
        Vk::RenderTarget<Res_VoxelLight> voxelLight;
        Vk::RenderTarget<Res_VoxelInt> voxelIntegrated;
        Vk::RenderTarget<Res_VoxelHist> voxelHistory;
        Vk::RenderTarget<Res_VoxelResolved> voxelResolved;
        Vk::RenderTarget<Res_TransNorm> transNormalBuffer;
        Vk::RenderTarget<Res_TransAnisotropy> transAnisotropyBuffer;
        Vk::RenderTarget<Res_TransSheen> transSheenBuffer;
        Vk::RenderTarget<Res_TransDepth> transDepthBuffer;

        Vk::RenderTarget<Res_TransLighting> transLightingTarget;
        Vk::RenderTarget<Res_HiZ> hizMap;

    };
    static constexpr uint32_t kShadowResolution = 2048;
    static constexpr uint32_t kCascades         = 4;

    static constexpr uint32_t kShadowAtlasSize = 4096;

    TargetManager(auto& ctx, auto& allocator, auto& ring) noexcept: _ctx(ctx), _allocator(allocator), _ring(ring) {
    }
    ~TargetManager() {
        Clear();
    }

    TargetManager(const TargetManager&)                        = delete;
    auto operator=(const TargetManager&) -> TargetManager&     = delete;
    TargetManager(TargetManager&&) noexcept                    = delete;
    auto operator=(TargetManager&&) noexcept -> TargetManager& = delete;

    [[nodiscard]] auto Recreate(VkExtent2D ext, VkExtent3D voxelExtent) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto InitShadows() -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto ResizeShadows(uint32_t resolution) noexcept -> std::expected<void, ErrorCode>;

    void RecordInitialLayouts(VkCommandBuffer cmd) const noexcept;

    void NameGraphTargets() const noexcept;

    void Clear() noexcept;

    [[nodiscard]] auto Graph() noexcept -> GraphResources& {
        return _graph;
    }
    [[nodiscard]] auto Graph() const noexcept -> const GraphResources& {
        return _graph;
    }

    [[nodiscard]] auto ShadowMapPrev() noexcept -> Vk::RenderTarget<Res_ShadowMap>& {
        return _shadowMapPrev;
    }
    [[nodiscard]] auto ShadowMapPrev() const noexcept -> const Vk::RenderTarget<Res_ShadowMap>& {
        return _shadowMapPrev;
    }

    [[nodiscard]] auto CascadeViews() const noexcept -> std::span<const Vk::AttachmentView> {
        return _shadowCascadeViews;
    }
    [[nodiscard]] auto CascadeViewsPrev() const noexcept -> std::span<const Vk::AttachmentView> {
        return _shadowCascadeViewsPrev;
    }
    void SwapCascadeViews() noexcept {
        std::swap(_shadowCascadeViews, _shadowCascadeViewsPrev);
    }

    [[nodiscard]] auto AtlasView() noexcept -> Vk::AttachmentView& {
        return _shadowAtlasView;
    }
    [[nodiscard]] auto AtlasView() const noexcept -> const Vk::AttachmentView& {
        return _shadowAtlasView;
    }

  private:
    [[nodiscard]] auto CreateCascadeViews(VkDevice device, const Vk::Image& image) const -> std::expected<ZHLN::Array<Vk::AttachmentView>, ErrorCode>;

    Vk::Context&                                 _ctx;
    Vk::Allocator&                               _allocator;
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& _ring;

    GraphResources _graph;

    Vk::RenderTarget<Res_ShadowMap> _shadowMapPrev;
    ZHLN::Array<Vk::AttachmentView>        _shadowCascadeViews;
    ZHLN::Array<Vk::AttachmentView>        _shadowCascadeViewsPrev;
    Vk::AttachmentView                     _shadowAtlasView;
};

// The two cascade images are swapped each frame. Keep their C++ types equal,
// even though one is graph-bound and the other is retained for the next frame.
static_assert(std::is_same_v<
    decltype(std::declval<TargetManager::GraphResources&>().shadowMap),
    std::remove_reference_t<decltype(std::declval<TargetManager&>().ShadowMapPrev())>>);

} // namespace ZHLN

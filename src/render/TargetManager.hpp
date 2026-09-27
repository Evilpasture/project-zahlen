// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "Rendering.hpp"
#include "graph/RenderGraph.hpp"
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

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor    = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity      = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough     = Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Emissive      = Vk::GraphImage<"Emissive", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Clearcoat     = Vk::GraphImage<"Clearcoat", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Depth         = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_ShadowMap     = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas   = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Lighting      = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HdrSceneColor = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_DenoiseA      = Vk::GraphImage<"DenoiseA", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_DenoiseB      = Vk::GraphImage<"DenoiseB", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_RtrHalf       = Vk::GraphImage<"RtrHalf", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_Ao            = Vk::GraphImage<"Ao", VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomThresh   = Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomDown1    = Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomDown2    = Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomDown3    = Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 16>;
using Res_BloomUp2      = Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomUp1      = Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomFinal    = Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_SmaaEdge      = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight    = Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain     = Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;
using Res_VoxelMedia    = Vk::GraphImage<"VoxelMedia", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelLight    = Vk::GraphImage<"VoxelLight", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelInt      = Vk::GraphImage<"VoxelInt", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelHist     = Vk::GraphImage<"VoxelHist", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 1, true>;
using Res_VoxelResolved = Vk::GraphImage<"VoxelResolved", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_TransNorm     = Vk::GraphImage<"TransNorm", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_TransDepth    = Vk::GraphImage<"TransDepth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_TransLighting = Vk::GraphImage<"TransLighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HiZ           = Vk::GraphImage<"HiZMap", VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
}

using Res_AccumCurr = Vk::GraphImage<"AccumCurr", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_AccumNext = Vk::GraphImage<"AccumNext", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;



template <VkFormat F>
[[nodiscard]] auto CreateColorTarget(Vk::Allocator& allocator, Vk::Context& ctx, VkExtent2D ext, Vk::ImageUsage extra = Vk::ImageUsage::None)
    -> std::expected<Vk::RenderTarget<F>, ErrorCode> {
    return Vk::RenderTarget<F>::Create(allocator, ctx, ext, {.usage = Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | extra});
}

class TargetManager {
  public:
    struct GraphResources {
        Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> sceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>           velocityBuffer;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          normalRoughnessBuffer;
        Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> emissiveBuffer;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          clearcoatBuffer;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     lightingTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     hdrSceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseA;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseB;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtrHalf;
        Vk::RenderTarget<VK_FORMAT_R8_UNORM>                ao;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomThresholdTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown1;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown2;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomDown3;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomUp2;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomUp1;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomFinalTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     bloomBlurTarget;
        Vk::RenderTarget<VK_FORMAT_R8G8_UNORM>              smaaEdgeTarget;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          smaaWeightTarget;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>              shadowMap;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>              shadowAtlas;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelMedia;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelLight;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelIntegrated;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelHistory;
        Vk::RenderTarget3D<VK_FORMAT_R16G16B16A16_SFLOAT>   voxelResolved;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          transNormalBuffer;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>      transDepthBuffer;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     transLightingTarget;
        Vk::MipmappedRenderTarget<VK_FORMAT_R32_SFLOAT>     hizMap;

        struct ReflectMetadata {
            Res_SceneColor    sceneColor;
            Res_Velocity      velocityBuffer;
            Res_NormRough     normalRoughnessBuffer;
            Res_Emissive      emissiveBuffer;
            Res_Clearcoat     clearcoatBuffer;
            Res_Lighting      lightingTarget;
            Res_HdrSceneColor hdrSceneColor;
            Res_DenoiseA      denoiseA;
            Res_DenoiseB      denoiseB;
            Res_RtrHalf       rtrHalf;
            Res_Ao            ao;
            Res_BloomThresh   bloomThresholdTarget;
            Res_BloomDown1    bloomDown1;
            Res_BloomDown2    bloomDown2;
            Res_BloomDown3    bloomDown3;
            Res_BloomUp2      bloomUp2;
            Res_BloomUp1      bloomUp1;
            Res_BloomFinal    bloomFinalTarget;
            Res_SmaaEdge      smaaEdgeTarget;
            Res_SmaaWeight    smaaWeightTarget;
            Res_ShadowAtlas   shadowAtlas;
            Res_VoxelMedia    voxelMedia;
            Res_VoxelLight    voxelLight;
            Res_VoxelInt      voxelIntegrated;
            Res_VoxelHist     voxelHistory;
            Res_VoxelResolved voxelResolved;
            Res_TransNorm     transNormalBuffer;
            Res_TransDepth    transDepthBuffer;
            Res_TransLighting transLightingTarget;
            Res_HiZ           hizMap;
        };
    };
    static constexpr uint32_t kShadowResolution = 2048;
    static constexpr uint32_t kCascades         = 4;
    static constexpr uint32_t kPunctualLights   = 4;
    static constexpr uint32_t kAtlasLayers      = 24;
    static constexpr uint32_t kAtlasSize        = 1024;

    TargetManager(Vk::Context& ctx, Vk::Allocator& allocator, Vk::CommandRing<Vk::QueueType::Graphics, 8>& ring) noexcept
        : _ctx(ctx), _allocator(allocator), _ring(ring) {}
    ~TargetManager() = default;

    TargetManager(const TargetManager&)                = delete;
    auto operator=(const TargetManager&) -> TargetManager& = delete;
    TargetManager(TargetManager&&) noexcept            = delete;
    auto operator=(TargetManager&&) noexcept -> TargetManager& = delete;


    [[nodiscard]] auto Recreate(VkExtent2D ext, VkExtent3D voxelExtent) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto InitShadows() -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto ResizeShadows(uint32_t resolution) noexcept -> std::expected<void, ErrorCode>;

    void RecreatePunctualShadowViews() noexcept;


    void RecordInitialLayouts(VkCommandBuffer cmd) const noexcept;

    void NameGraphTargets() const noexcept;


    [[nodiscard]] auto Graph() noexcept -> GraphResources& { return _graph; }
    [[nodiscard]] auto Graph() const noexcept -> const GraphResources& { return _graph; }

    [[nodiscard]] auto ShadowMapPrev() noexcept -> Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>& { return _shadowMapPrev; }
    [[nodiscard]] auto ShadowMapPrev() const noexcept -> const Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>& { return _shadowMapPrev; }

    [[nodiscard]] auto CascadeViews() noexcept -> ZHLN::Array<Vk::ImageView>& { return _shadowCascadeViews; }
    [[nodiscard]] auto CascadeViews() const noexcept -> const ZHLN::Array<Vk::ImageView>& { return _shadowCascadeViews; }
    [[nodiscard]] auto CascadeViewsPrev() noexcept -> ZHLN::Array<Vk::ImageView>& { return _shadowCascadeViewsPrev; }
    [[nodiscard]] auto CascadeViewsPrev() const noexcept -> const ZHLN::Array<Vk::ImageView>& { return _shadowCascadeViewsPrev; }
    [[nodiscard]] auto PunctualViews() noexcept -> ZHLN::Array<Vk::ImageView>& { return _punctualShadowViews; }
    [[nodiscard]] auto PunctualViews() const noexcept -> const ZHLN::Array<Vk::ImageView>& { return _punctualShadowViews; }

    [[nodiscard]] auto AtlasCubeView() noexcept -> Vk::ImageView& { return _shadowAtlasCubeView; }
    [[nodiscard]] auto AtlasCubeView() const noexcept -> const Vk::ImageView& { return _shadowAtlasCubeView; }
    [[nodiscard]] auto Atlas2DView() noexcept -> Vk::ImageView& { return _shadowAtlas2DView; }
    [[nodiscard]] auto Atlas2DView() const noexcept -> const Vk::ImageView& { return _shadowAtlas2DView; }
    [[nodiscard]] auto AtlasCubeViewInfo() noexcept -> VkImageViewCreateInfo& { return _shadowAtlasCubeViewInfo; }
    [[nodiscard]] auto AtlasCubeViewInfo() const noexcept -> const VkImageViewCreateInfo& { return _shadowAtlasCubeViewInfo; }
    [[nodiscard]] auto Atlas2DViewInfo() noexcept -> VkImageViewCreateInfo& { return _shadowAtlas2DViewInfo; }
    [[nodiscard]] auto Atlas2DViewInfo() const noexcept -> const VkImageViewCreateInfo& { return _shadowAtlas2DViewInfo; }

  private:
    [[nodiscard]] auto CreateCascadeViews(VkImage image, ZHLN::Array<Vk::ImageView>& out) const -> std::expected<void, ErrorCode>;

    Vk::Context&                             _ctx;
    Vk::Allocator&                           _allocator;
    Vk::CommandRing<Vk::QueueType::Graphics, 8>& _ring;

    GraphResources _graph;

    Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> _shadowMapPrev;
    ZHLN::Array<Vk::ImageView>             _shadowCascadeViews;
    ZHLN::Array<Vk::ImageView>             _shadowCascadeViewsPrev;
    ZHLN::Array<Vk::ImageView>             _punctualShadowViews;
    Vk::ImageView                          _shadowAtlasCubeView;
    Vk::ImageView                          _shadowAtlas2DView;
    VkImageViewCreateInfo                  _shadowAtlasCubeViewInfo {};
    VkImageViewCreateInfo                  _shadowAtlas2DViewInfo {};
};

}

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

using Res_SceneColor = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity   = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough  = Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;

using Res_Depth         = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_ShadowMap     = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas   = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Lighting      = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HdrSceneColor = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_DenoiseA      = Vk::GraphImage<"DenoiseA", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_DenoiseB      = Vk::GraphImage<"DenoiseB", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
// The RT GI, RT reflection and RT shadow chains all trace and filter below
// native resolution; the shadow chain was the last one still running at full
// resolution, which is why ShadowDenoise cost more than the shadow trace it
// filters. Kept as one constant because the shaders hard-code the inverse as
// kShadowTraceScale, and a mismatch between the two silently misaligns the
// depth and normal taps against the filtered signal.
inline constexpr uint32_t kShadowTraceDivisor = 2;

using Res_RtShadowMask    = Vk::GraphImage<"RtShadowMask", VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, kShadowTraceDivisor>;
using Res_RtShadowHistory = Vk::GraphImage<"RtShadowHistory", VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, kShadowTraceDivisor>;
using Res_ShadowDenoiseA  = Vk::GraphImage<"ShadowDenoiseA", VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, kShadowTraceDivisor>;
using Res_ShadowDenoiseB  = Vk::GraphImage<"ShadowDenoiseB", VK_FORMAT_R16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, kShadowTraceDivisor>;
using Res_RtrHalf         = Vk::GraphImage<"RtrHalf", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 2>;
using Res_RtrHistory      = Vk::GraphImage<"RtrHistory", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 2>;
using Res_RtgiHalf        = Vk::GraphImage<"RtgiHalf", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 2>;
using Res_RtgiHistory     = Vk::GraphImage<"RtgiHistory", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 2>;
using Res_RtgiDenoiseA    = Vk::GraphImage<"RtgiDenoiseA", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_RtgiDenoiseB    = Vk::GraphImage<"RtgiDenoiseB", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_DdgiIrradiance  = Vk::GraphImage<"DdgiIrradiance", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_DdgiDistance    = Vk::GraphImage<"DdgiDistance", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_DdgiRays        = Vk::GraphImage<"DdgiRays", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_LightingGI      = Vk::GraphImage<"LightingGI", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_LightingAo      = Vk::GraphImage<"LightingAo", VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Ao              = Vk::GraphImage<"Ao", VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomThresh     = Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomDown1      = Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomDown2      = Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomDown3      = Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 16>;
using Res_BloomUp2        = Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomUp1        = Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomFinal      = Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_SmaaEdge        = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight      = Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain       = Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;
using Res_VoxelMedia      = Vk::GraphImage<"VoxelMedia", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelLight      = Vk::GraphImage<"VoxelLight", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelInt        = Vk::GraphImage<"VoxelInt", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelHist       = Vk::GraphImage<"VoxelHist", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 1, true>;
using Res_VoxelResolved   = Vk::GraphImage<"VoxelResolved", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_TransNorm       = Vk::GraphImage<"TransNorm", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_TransSheen      = Vk::GraphImage<"TransSheen", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_TransAnisotropy = Vk::GraphImage<"TransAnisotropy", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_TransDepth      = Vk::GraphImage<"TransDepth", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT>;
using Res_TransLighting   = Vk::GraphImage<"TransLighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_HiZ             = Vk::GraphImage<"HiZMap", VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace Vk

using Res_AccumPrevious = Vk::GraphImage<"AccumPrevious", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_AccumCurrent  = Vk::GraphImage<"AccumCurrent", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;

template <VkFormat F>
[[nodiscard]] auto CreateColorTarget(Vk::Allocator& allocator, VkDevice device, VkExtent2D ext, Vk::ImageUsage extra = Vk::ImageUsage::None)
    -> std::expected<Vk::RenderTarget<F>, ErrorCode> {
    return Vk::RenderTarget<F>::Create(allocator, device, ext, {.usage = Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | extra});
}

class TargetManager {
  public:
    struct GraphResources {
        Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> sceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>           velocityBuffer;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          normalRoughnessBuffer;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     lightingTarget;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     hdrSceneColor;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseA;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     denoiseB;
        Vk::RenderTarget<VK_FORMAT_R16_SFLOAT>              rtShadowMask;
        Vk::RenderTarget<VK_FORMAT_R16_SFLOAT>              rtShadowHistory;
        Vk::RenderTarget<VK_FORMAT_R16_SFLOAT>              shadowDenoiseA;
        Vk::RenderTarget<VK_FORMAT_R16_SFLOAT>              shadowDenoiseB;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtrHalf;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtrHistory;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtgiHalf;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtgiHistory;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtgiDenoiseA;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     rtgiDenoiseB;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     ddgiIrradiance;
        Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>           ddgiDistance;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     ddgiRays;
        Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>     lightingGI;
        Vk::RenderTarget<VK_FORMAT_R8_UNORM>                lightingAo;
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
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          transAnisotropyBuffer;
        Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>          transSheenBuffer;
        Vk::RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>      transDepthBuffer;

        Vk::MipmappedRenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> transLightingTarget;
        Vk::MipmappedRenderTarget<VK_FORMAT_R32_SFLOAT>          hizMap;

        struct ReflectMetadata {
            Res_SceneColor      sceneColor;
            Res_Velocity        velocityBuffer;
            Res_NormRough       normalRoughnessBuffer;
            Res_Lighting        lightingTarget;
            Res_HdrSceneColor   hdrSceneColor;
            Res_DenoiseA        denoiseA;
            Res_DenoiseB        denoiseB;
            Res_RtShadowMask    rtShadowMask;
            Res_RtShadowHistory rtShadowHistory;
            Res_ShadowDenoiseA  shadowDenoiseA;
            Res_ShadowDenoiseB  shadowDenoiseB;
            Res_RtrHalf         rtrHalf;
            Res_RtrHistory      rtrHistory;
            Res_RtgiHalf        rtgiHalf;
            Res_RtgiHistory     rtgiHistory;
            Res_RtgiDenoiseA    rtgiDenoiseA;
            Res_RtgiDenoiseB    rtgiDenoiseB;
            Res_DdgiIrradiance  ddgiIrradiance;
            Res_DdgiDistance    ddgiDistance;
            Res_DdgiRays        ddgiRays;
            Res_LightingGI      lightingGI;
            Res_LightingAo      lightingAo;
            Res_Ao              ao;
            Res_BloomThresh     bloomThresholdTarget;
            Res_BloomDown1      bloomDown1;
            Res_BloomDown2      bloomDown2;
            Res_BloomDown3      bloomDown3;
            Res_BloomUp2        bloomUp2;
            Res_BloomUp1        bloomUp1;
            Res_BloomFinal      bloomFinalTarget;
            Res_SmaaEdge        smaaEdgeTarget;
            Res_SmaaWeight      smaaWeightTarget;
            Res_ShadowAtlas     shadowAtlas;
            Res_VoxelMedia      voxelMedia;
            Res_VoxelLight      voxelLight;
            Res_VoxelInt        voxelIntegrated;
            Res_VoxelHist       voxelHistory;
            Res_VoxelResolved   voxelResolved;
            Res_TransNorm       transNormalBuffer;
            Res_TransAnisotropy transAnisotropyBuffer;
            Res_TransSheen      transSheenBuffer;
            Res_TransDepth      transDepthBuffer;
            Res_TransLighting   transLightingTarget;
            Res_HiZ             hizMap;
        };
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

    [[nodiscard]] auto ShadowMapPrev() noexcept -> Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>& {
        return _shadowMapPrev;
    }
    [[nodiscard]] auto ShadowMapPrev() const noexcept -> const Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>& {
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

    Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> _shadowMapPrev;
    ZHLN::Array<Vk::AttachmentView>        _shadowCascadeViews;
    ZHLN::Array<Vk::AttachmentView>        _shadowCascadeViewsPrev;
    Vk::AttachmentView                     _shadowAtlasView;
};

} // namespace ZHLN

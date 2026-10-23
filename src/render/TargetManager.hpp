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

using Res_SceneColor = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32>;
using Res_Velocity   = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT>;
using Res_NormRough  = Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM>;
// Which shading model owns the static lighting of a pixel, as a bitfield:
//   bit 0 - a compile-time bake supplied this surface's diffuse ambient (a VRAD
//           lightmap lump, or the leaf ambient cube that stands in for one on
//           props), so the deferred pass's analytic SH ambient would be a
//           second helping rather than a base layer,
//   bit 1 - that same bake owns *direct* light too, which only a lightmap does:
//           a cube is indirect-only, so a prop riding one keeps its live sun.
// Zero means the analytic ambient/GI and every live light are all the pixel
// gets, and both are applied in full.
// Three of the compact G-buffer's channels are already spoken for (octahedral
// normal, roughness, metallic) and a fourth (scene color) has no alpha lane, so
// the ownership bits get their own attachment rather than stealing precision
// from a lane that shading reads. UINT, not UNORM, because these are bits: an
// 8-bit UNORM lane can only hold 1.0 or 0.0 without inventing an encoding, and
// two independent flags have no exact UNORM spelling. Consumed by
// lighting.slang only.
using Res_BakedMask  = Vk::GraphImage<"BakedMask", VK_FORMAT_R8_UINT>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth      = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT>;

using Res_ShadowMap     = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT>;
using Res_ShadowAtlas   = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT>;
using Res_Lighting      = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_HdrSceneColor = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_DenoiseA      = Vk::GraphImage<"DenoiseA", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_DenoiseB      = Vk::GraphImage<"DenoiseB", VK_FORMAT_R16G16B16A16_SFLOAT>;
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
using Res_DdgiRays        = Vk::GraphImage<"DdgiRays", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_LightingGI      = Vk::GraphImage<"LightingGI", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_LightingAo      = Vk::GraphImage<"LightingAo", VK_FORMAT_R8_UNORM>;
using Res_Ao              = Vk::GraphImage<"Ao", VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomThresh     = Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_BloomDown1      = Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomDown2      = Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomDown3      = Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 16>;
using Res_BloomUp2        = Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 8>;
using Res_BloomUp1        = Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 4>;
using Res_BloomFinal      = Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 2>;
using Res_SmaaEdge        = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM>;
using Res_SmaaWeight      = Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM>;
using Res_Swapchain       = Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;
using Res_VoxelMedia      = Vk::GraphImage<"VoxelMedia", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelLight      = Vk::GraphImage<"VoxelLight", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelInt        = Vk::GraphImage<"VoxelInt", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_VoxelHist       = Vk::GraphImage<"VoxelHist", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true, 1, true>;
using Res_VoxelResolved   = Vk::GraphImage<"VoxelResolved", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, true>;
using Res_TransNorm       = Vk::GraphImage<"TransNorm", VK_FORMAT_R8G8B8A8_UNORM>;
using Res_TransSheen      = Vk::GraphImage<"TransSheen", VK_FORMAT_R8G8B8A8_UNORM>;
using Res_TransAnisotropy = Vk::GraphImage<"TransAnisotropy", VK_FORMAT_R8G8B8A8_UNORM>;
using Res_TransDepth      = Vk::GraphImage<"TransDepth", VK_FORMAT_D32_SFLOAT_S8_UINT>;
using Res_TransLighting   = Vk::GraphImage<"TransLighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, false, true>;
using Res_HiZ             = Vk::GraphImage<"HiZMap", VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, false, true>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace Vk

using Res_AccumPrevious = Vk::GraphImage<"AccumPrevious", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;
using Res_AccumCurrent  = Vk::GraphImage<"AccumCurrent", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, true>;

class TargetManager {
  public:
    struct GraphResources {
        Vk::RenderTarget<Res_SceneColor> sceneColor;
        Vk::RenderTarget<Res_Velocity> velocityBuffer;
        Vk::RenderTarget<Res_NormRough> normalRoughnessBuffer;
        Vk::RenderTarget<Res_BakedMask> bakedMask;
        Vk::RenderTarget<Res_Lighting> lightingTarget;
        Vk::RenderTarget<Res_HdrSceneColor> hdrSceneColor;
        Vk::RenderTarget<Res_DenoiseA> denoiseA;
        Vk::RenderTarget<Res_DenoiseB> denoiseB;
        Vk::RenderTarget<Res_RtShadowMask> rtShadowMask;
        Vk::RenderTarget<Res_RtShadowHistory> rtShadowHistory;
        Vk::RenderTarget<Res_ShadowDenoiseA> shadowDenoiseA;
        Vk::RenderTarget<Res_ShadowDenoiseB> shadowDenoiseB;
        Vk::RenderTarget<Res_RtrHalf> rtrHalf;
        Vk::RenderTarget<Res_RtrHistory> rtrHistory;
        Vk::RenderTarget<Res_RtgiHalf> rtgiHalf;
        Vk::RenderTarget<Res_RtgiHistory> rtgiHistory;
        Vk::RenderTarget<Res_RtgiDenoiseA> rtgiDenoiseA;
        Vk::RenderTarget<Res_RtgiDenoiseB> rtgiDenoiseB;
        Vk::RenderTarget<Res_DdgiIrradiance> ddgiIrradiance;
        Vk::RenderTarget<Res_DdgiDistance> ddgiDistance;
        Vk::RenderTarget<Res_DdgiRays> ddgiRays;
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

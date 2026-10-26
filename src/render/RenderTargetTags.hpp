// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "graph/GraphImage.hpp"
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor = Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32>;
using Res_Velocity   = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT>;
using Res_NormRough  = Vk::GraphImage<"NormRough", VK_FORMAT_R16G16B16A16_SFLOAT>;
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
using Res_BakedMask = Vk::GraphImage<"BakedMask", VK_FORMAT_R8_UINT>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT>;

using Res_ShadowMap       = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT>;
using Res_ShadowAtlas     = Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT>;
using Res_Lighting        = Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_HdrSceneColor   = Vk::GraphImage<"HdrSceneColor", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_RcgiProbes      = Vk::GraphImage<"RcgiProbes", VK_FORMAT_R32G32B32A32_SFLOAT>;
using Res_RcgiCascadeA    = Vk::GraphImage<"RcgiCascadeA", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_RcgiCascadeB    = Vk::GraphImage<"RcgiCascadeB", VK_FORMAT_R16G16B16A16_SFLOAT>;
using Res_LightingGI      = Vk::GraphImage<"LightingGI", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, false, 1, false, true>;
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

} // namespace ZHLN

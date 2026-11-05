// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include <vkd3d/graph/GraphImage.hpp>
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor = VkD3D::GraphImage<"SceneColor", VkD3D::Format::B10G11R11UfloatPack32>;
using Res_Velocity   = VkD3D::GraphImage<"Velocity", VkD3D::Format::R16G16Sfloat>;
using Res_NormRough  = VkD3D::GraphImage<"NormRough", VkD3D::Format::R16G16B16A16Sfloat>;
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
using Res_BakedMask = VkD3D::GraphImage<"BakedMask", VkD3D::Format::R8Uint>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth = VkD3D::GraphImage<"Depth", VkD3D::Format::D32SfloatS8Uint>;

using Res_ShadowMap       = VkD3D::GraphImage<"ShadowMap", VkD3D::Format::D32Sfloat>;
using Res_ShadowAtlas     = VkD3D::GraphImage<"ShadowAtlas", VkD3D::Format::D32Sfloat>;
using Res_Lighting        = VkD3D::GraphImage<"Lighting", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_HdrSceneColor   = VkD3D::GraphImage<"HdrSceneColor", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_RcgiProbes      = VkD3D::GraphImage<"RcgiProbes", VkD3D::Format::R32G32B32A32Sfloat>;
using Res_RcgiCascadeA    = VkD3D::GraphImage<"RcgiCascadeA", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_RcgiCascadeB    = VkD3D::GraphImage<"RcgiCascadeB", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_LightingGI      = VkD3D::GraphImage<"LightingGI", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;
using Res_LightingAo      = VkD3D::GraphImage<"LightingAo", VkD3D::Format::R8Unorm>;
using Res_Ao              = VkD3D::GraphImage<"Ao", VkD3D::Format::R8Unorm, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_BloomThresh     = VkD3D::GraphImage<"BloomThresh", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_BloomDown1      = VkD3D::GraphImage<"BloomDown1", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 4>;
using Res_BloomDown2      = VkD3D::GraphImage<"BloomDown2", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 8>;
using Res_BloomDown3      = VkD3D::GraphImage<"BloomDown3", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 16>;
using Res_BloomUp2        = VkD3D::GraphImage<"BloomUp2", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 8>;
using Res_BloomUp1        = VkD3D::GraphImage<"BloomUp1", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 4>;
using Res_BloomFinal      = VkD3D::GraphImage<"BloomFinal", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_SmaaEdge        = VkD3D::GraphImage<"SmaaEdge", VkD3D::Format::R8G8Unorm>;
using Res_SmaaWeight      = VkD3D::GraphImage<"SmaaWeight", VkD3D::Format::R8G8B8A8Unorm>;
using Res_Swapchain       = VkD3D::GraphImage<"Swapchain", VkD3D::Format::B8G8R8A8Srgb, VkD3D::ImageAspect::Color, true>;
using Res_VoxelMedia      = VkD3D::GraphImage<"VoxelMedia", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelLight      = VkD3D::GraphImage<"VoxelLight", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelInt        = VkD3D::GraphImage<"VoxelInt", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_TransNorm       = VkD3D::GraphImage<"TransNorm", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransSheen      = VkD3D::GraphImage<"TransSheen", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransAnisotropy = VkD3D::GraphImage<"TransAnisotropy", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransDepth      = VkD3D::GraphImage<"TransDepth", VkD3D::Format::D32SfloatS8Uint>;
using Res_TransLighting   = VkD3D::GraphImage<"TransLighting", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;
using Res_HiZ             = VkD3D::GraphImage<"HiZMap", VkD3D::Format::R32Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;

namespace VkD3D {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace VkD3D

using Res_AccumPrevious = VkD3D::GraphImage<"AccumPrevious", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, true>;
using Res_AccumCurrent  = VkD3D::GraphImage<"AccumCurrent", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, true>;

} // namespace ZHLN

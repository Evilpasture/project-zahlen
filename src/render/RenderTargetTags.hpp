// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "graph/GraphImage.hpp"
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor = Vk::GraphImageTag<"SceneColor", VkD3D::Format::B10G11R11UfloatPack32>;
using Res_Velocity   = Vk::GraphImageTag<"Velocity", VkD3D::Format::R16G16Sfloat>;
using Res_NormRough  = Vk::GraphImageTag<"NormRough", VkD3D::Format::R16G16B16A16Sfloat>;
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
using Res_BakedMask = Vk::GraphImageTag<"BakedMask", VkD3D::Format::R8Uint>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth = Vk::GraphImageTag<"Depth", VkD3D::Format::D32SfloatS8Uint>;

using Res_ShadowMap       = Vk::GraphImageTag<"ShadowMap", VkD3D::Format::D32Sfloat>;
using Res_ShadowAtlas     = Vk::GraphImageTag<"ShadowAtlas", VkD3D::Format::D32Sfloat>;
using Res_Lighting        = Vk::GraphImageTag<"Lighting", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_HdrSceneColor   = Vk::GraphImageTag<"HdrSceneColor", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_RcgiProbes      = Vk::GraphImageTag<"RcgiProbes", VkD3D::Format::R32G32B32A32Sfloat>;
using Res_RcgiCascadeA    = Vk::GraphImageTag<"RcgiCascadeA", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_RcgiCascadeB    = Vk::GraphImageTag<"RcgiCascadeB", VkD3D::Format::R16G16B16A16Sfloat>;
using Res_LightingGI      = Vk::GraphImageTag<"LightingGI", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;
using Res_LightingAo      = Vk::GraphImageTag<"LightingAo", VkD3D::Format::R8Unorm>;
using Res_Ao              = Vk::GraphImageTag<"Ao", VkD3D::Format::R8Unorm, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_BloomThresh     = Vk::GraphImageTag<"BloomThresh", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_BloomDown1      = Vk::GraphImageTag<"BloomDown1", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 4>;
using Res_BloomDown2      = Vk::GraphImageTag<"BloomDown2", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 8>;
using Res_BloomDown3      = Vk::GraphImageTag<"BloomDown3", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 16>;
using Res_BloomUp2        = Vk::GraphImageTag<"BloomUp2", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 8>;
using Res_BloomUp1        = Vk::GraphImageTag<"BloomUp1", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 4>;
using Res_BloomFinal      = Vk::GraphImageTag<"BloomFinal", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 2>;
using Res_SmaaEdge        = Vk::GraphImageTag<"SmaaEdge", VkD3D::Format::R8G8Unorm>;
using Res_SmaaWeight      = Vk::GraphImageTag<"SmaaWeight", VkD3D::Format::R8G8B8A8Unorm>;
using Res_Swapchain       = Vk::GraphImageTag<"Swapchain", VkD3D::Format::B8G8R8A8Srgb, VkD3D::ImageAspect::Color, true>;
using Res_VoxelMedia      = Vk::GraphImageTag<"VoxelMedia", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelLight      = Vk::GraphImageTag<"VoxelLight", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelInt        = Vk::GraphImageTag<"VoxelInt", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, true>;
using Res_TransNorm       = Vk::GraphImageTag<"TransNorm", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransSheen      = Vk::GraphImageTag<"TransSheen", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransAnisotropy = Vk::GraphImageTag<"TransAnisotropy", VkD3D::Format::R8G8B8A8Unorm>;
using Res_TransDepth      = Vk::GraphImageTag<"TransDepth", VkD3D::Format::D32SfloatS8Uint>;
using Res_TransLighting   = Vk::GraphImageTag<"TransLighting", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;
using Res_HiZ             = Vk::GraphImageTag<"HiZMap", VkD3D::Format::R32Sfloat, VkD3D::ImageAspect::Color, false, false, 1, false, true>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace Vk

using Res_AccumPrevious = Vk::GraphImageTag<"AccumPrevious", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, true>;
using Res_AccumCurrent  = Vk::GraphImageTag<"AccumCurrent", VkD3D::Format::R16G16B16A16Sfloat, VkD3D::ImageAspect::Color, false, true>;

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "graph/GraphImage.hpp"
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor = Vk::GraphImage<"SceneColor", Vk::Format::B10G11R11UfloatPack32>;
using Res_Velocity   = Vk::GraphImage<"Velocity", Vk::Format::R16G16Sfloat>;
using Res_NormRough  = Vk::GraphImage<"NormRough", Vk::Format::R16G16B16A16Sfloat>;
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
using Res_BakedMask = Vk::GraphImage<"BakedMask", Vk::Format::R8Uint>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth = Vk::GraphImage<"Depth", Vk::Format::D32SfloatS8Uint>;

using Res_ShadowMap       = Vk::GraphImage<"ShadowMap", Vk::Format::D32Sfloat>;
using Res_ShadowAtlas     = Vk::GraphImage<"ShadowAtlas", Vk::Format::D32Sfloat>;
using Res_Lighting        = Vk::GraphImage<"Lighting", Vk::Format::R16G16B16A16Sfloat>;
using Res_HdrSceneColor   = Vk::GraphImage<"HdrSceneColor", Vk::Format::R16G16B16A16Sfloat>;
using Res_RcgiProbes      = Vk::GraphImage<"RcgiProbes", Vk::Format::R32G32B32A32Sfloat>;
using Res_RcgiCascadeA    = Vk::GraphImage<"RcgiCascadeA", Vk::Format::R16G16B16A16Sfloat>;
using Res_RcgiCascadeB    = Vk::GraphImage<"RcgiCascadeB", Vk::Format::R16G16B16A16Sfloat>;
using Res_LightingGI      = Vk::GraphImage<"LightingGI", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 1, false, true>;
using Res_LightingAo      = Vk::GraphImage<"LightingAo", Vk::Format::R8Unorm>;
using Res_Ao              = Vk::GraphImage<"Ao", Vk::Format::R8Unorm, Vk::ImageAspect::Color, false, false, 2>;
using Res_BloomThresh     = Vk::GraphImage<"BloomThresh", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 2>;
using Res_BloomDown1      = Vk::GraphImage<"BloomDown1", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 4>;
using Res_BloomDown2      = Vk::GraphImage<"BloomDown2", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 8>;
using Res_BloomDown3      = Vk::GraphImage<"BloomDown3", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 16>;
using Res_BloomUp2        = Vk::GraphImage<"BloomUp2", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 8>;
using Res_BloomUp1        = Vk::GraphImage<"BloomUp1", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 4>;
using Res_BloomFinal      = Vk::GraphImage<"BloomFinal", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 2>;
using Res_SmaaEdge        = Vk::GraphImage<"SmaaEdge", Vk::Format::R8G8Unorm>;
using Res_SmaaWeight      = Vk::GraphImage<"SmaaWeight", Vk::Format::R8G8B8A8Unorm>;
using Res_Swapchain       = Vk::GraphImage<"Swapchain", Vk::Format::B8G8R8A8Srgb, Vk::ImageAspect::Color, true>;
using Res_VoxelMedia      = Vk::GraphImage<"VoxelMedia", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelLight      = Vk::GraphImage<"VoxelLight", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 1, true>;
using Res_VoxelInt        = Vk::GraphImage<"VoxelInt", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 1, true>;
using Res_TransNorm       = Vk::GraphImage<"TransNorm", Vk::Format::R8G8B8A8Unorm>;
using Res_TransSheen      = Vk::GraphImage<"TransSheen", Vk::Format::R8G8B8A8Unorm>;
using Res_TransAnisotropy = Vk::GraphImage<"TransAnisotropy", Vk::Format::R8G8B8A8Unorm>;
using Res_TransDepth      = Vk::GraphImage<"TransDepth", Vk::Format::D32SfloatS8Uint>;
using Res_TransLighting   = Vk::GraphImage<"TransLighting", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, false, 1, false, true>;
using Res_HiZ             = Vk::GraphImage<"HiZMap", Vk::Format::R32Sfloat, Vk::ImageAspect::Color, false, false, 1, false, true>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
} // namespace Vk

using Res_AccumPrevious = Vk::GraphImage<"AccumPrevious", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, true>;
using Res_AccumCurrent  = Vk::GraphImage<"AccumCurrent", Vk::Format::R16G16B16A16Sfloat, Vk::ImageAspect::Color, false, true>;

} // namespace ZHLN

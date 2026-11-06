// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "render/RenderConstants.hpp"
#include "vulkan/Rendering.hpp"
#include "vulkan/FormatTags.hpp"
#include "vulkan/graph/GraphImage.hpp"
#include <cstdint>

namespace ZHLN {

inline constexpr uint32_t kMaxGeneratedHiZMips = 7;

using Res_SceneColor = Vk::GraphImage<"SceneColor", Vk::Format::B10G11R11_UFloatPack32>;
using Res_Velocity   = Vk::GraphImage<"Velocity", Vk::Format::R16G16_SFloat>;
using Res_NormRough  = Vk::GraphImage<"NormRough", Vk::Format::R16G16B16A16_SFloat>;
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
using Res_BakedMask = Vk::GraphImage<"BakedMask", Vk::Format::R8_UInt>;
// The active presentation supplies backing storage; the resource identity is renderer-owned.
using Res_Depth = Vk::GraphImage<"Depth", Vk::Format::D32_SFloat_S8_UInt>;

using Res_ShadowMap       = Vk::GraphImage<"ShadowMap", Vk::Format::D32_SFloat>;
using Res_ShadowAtlas     = Vk::GraphImage<"ShadowAtlas", Vk::Format::D32_SFloat>;
using Res_Lighting        = Vk::GraphImage<"Lighting", Vk::Format::R16G16B16A16_SFloat>;
using Res_HdrSceneColor   = Vk::GraphImage<"HdrSceneColor", Vk::Format::R16G16B16A16_SFloat>;
using Res_RcgiProbes      = Vk::GraphImage<"RcgiProbes", Vk::Format::R32G32B32A32_SFloat>;
using Res_RcgiCascadeA    = Vk::GraphImage<"RcgiCascadeA", Vk::Format::R16G16B16A16_SFloat>;
using Res_RcgiCascadeB    = Vk::GraphImage<"RcgiCascadeB", Vk::Format::R16G16B16A16_SFloat>;
using Res_LightingGI      = Vk::GraphImage<"LightingGI", Vk::Format::R16G16B16A16_SFloat, false, false, 1, false, true>;
using Res_LightingAo      = Vk::GraphImage<"LightingAo", Vk::Format::R8_UNorm>;
using Res_Ao              = Vk::GraphImage<"Ao", Vk::Format::R8_UNorm, false, false, 2>;
using Res_BloomThresh     = Vk::GraphImage<"BloomThresh", Vk::Format::R16G16B16A16_SFloat, false, false, 2>;
using Res_BloomDown1      = Vk::GraphImage<"BloomDown1", Vk::Format::R16G16B16A16_SFloat, false, false, 4>;
using Res_BloomDown2      = Vk::GraphImage<"BloomDown2", Vk::Format::R16G16B16A16_SFloat, false, false, 8>;
using Res_BloomDown3      = Vk::GraphImage<"BloomDown3", Vk::Format::R16G16B16A16_SFloat, false, false, 16>;
using Res_BloomUp2        = Vk::GraphImage<"BloomUp2", Vk::Format::R16G16B16A16_SFloat, false, false, 8>;
using Res_BloomUp1        = Vk::GraphImage<"BloomUp1", Vk::Format::R16G16B16A16_SFloat, false, false, 4>;
using Res_BloomFinal      = Vk::GraphImage<"BloomFinal", Vk::Format::R16G16B16A16_SFloat, false, false, 2>;
using Res_SmaaEdge        = Vk::GraphImage<"SmaaEdge", Vk::Format::R8G8_UNorm>;
using Res_SmaaWeight      = Vk::GraphImage<"SmaaWeight", Vk::Format::R8G8B8A8_UNorm>;
using Res_Swapchain       = Vk::GraphImage<"Swapchain", Vk::Format::B8G8R8A8_SRGB, true>;
using Res_VoxelMedia      = Vk::GraphImage<"VoxelMedia", Vk::Format::R16G16B16A16_SFloat, false, false, 1, true>;
using Res_VoxelLight      = Vk::GraphImage<"VoxelLight", Vk::Format::R16G16B16A16_SFloat, false, false, 1, true>;
using Res_VoxelInt        = Vk::GraphImage<"VoxelInt", Vk::Format::R16G16B16A16_SFloat, false, false, 1, true>;
using Res_TransNorm       = Vk::GraphImage<"TransNorm", Vk::Format::R8G8B8A8_UNorm>;
using Res_TransSheen      = Vk::GraphImage<"TransSheen", Vk::Format::R8G8B8A8_UNorm>;
using Res_TransAnisotropy = Vk::GraphImage<"TransAnisotropy", Vk::Format::R8G8B8A8_UNorm>;
using Res_TransDepth      = Vk::GraphImage<"TransDepth", Vk::Format::D32_SFloat_S8_UInt>;
using Res_TransLighting   = Vk::GraphImage<"TransLighting", Vk::Format::R16G16B16A16_SFloat, false, false, 1, false, true>;
using Res_HiZ             = Vk::GraphImage<"HiZMap", Vk::Format::R32_SFloat, false, false, 1, false, true>;

namespace Vk {
template <>
struct ClearColorOf<Res_TransLighting> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};

// The graph clears an attachment from ClearColorOf<Tag>, whose default is
// opaque black {0,0,0,1}. These are the targets that are not cleared to that
// default -- the values below are the ones the passes were clearing to by hand,
// moved here so the pass that first writes the target no longer has to restate
// them (and cannot disagree with the graph about them).
//
// SceneColor is the visible background while the G-buffer is still empty, so it
// is the one clear that is not black. Velocity, NormRough and BakedMask clear to
// zero in every channel: those are data lanes, and alpha 1 would be read as a
// real value (see kClearColorBakedMask on why a zero Color4 is exact for the
// UINT lane). Anisotropy's alpha is likewise a lane, cleared to 1; TransNorm and
// TransSheen are not.
template <>
struct ClearColorOf<Res_SceneColor> {
    static constexpr Color4 value = {.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f};
};
template <>
struct ClearColorOf<Res_Velocity> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
template <>
struct ClearColorOf<Res_NormRough> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
template <>
struct ClearColorOf<Res_BakedMask> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
template <>
struct ClearColorOf<Res_TransNorm> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
template <>
struct ClearColorOf<Res_TransSheen> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
};
template <>
struct ClearColorOf<Res_TransAnisotropy> {
    static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};
};
} // namespace Vk

// The clear values above were moved out of the passes, so assert they still say
// what the passes say. kClearColor* stays the single definition; these read it,
// which also means a change lands in one place instead of two that can differ.
static_assert(Vk::ClearColorOf<Res_SceneColor>::value.r == kClearColorScene.r && Vk::ClearColorOf<Res_SceneColor>::value.g == kClearColorScene.g &&
              Vk::ClearColorOf<Res_SceneColor>::value.b == kClearColorScene.b && Vk::ClearColorOf<Res_SceneColor>::value.a == kClearColorScene.a);
static_assert(Vk::ClearColorOf<Res_Velocity>::value.a == kClearColorVelocity.a && Vk::ClearColorOf<Res_Velocity>::value.r == kClearColorVelocity.r);
static_assert(Vk::ClearColorOf<Res_NormRough>::value.a == kClearColorNormalRoughness.a && Vk::ClearColorOf<Res_NormRough>::value.r == kClearColorNormalRoughness.r);
static_assert(Vk::ClearColorOf<Res_BakedMask>::value.a == kClearColorBakedMask.a && Vk::ClearColorOf<Res_BakedMask>::value.r == kClearColorBakedMask.r);
static_assert(Vk::ClearColorOf<Res_TransNorm>::value.a == kClearColorNormalRoughness.a);
static_assert(Vk::ClearColorOf<Res_TransSheen>::value.a == kClearColorSheen.a && Vk::ClearColorOf<Res_TransSheen>::value.r == kClearColorSheen.r);
static_assert(Vk::ClearColorOf<Res_TransAnisotropy>::value.a == kClearColorAnisotropy.a && Vk::ClearColorOf<Res_TransAnisotropy>::value.r == kClearColorAnisotropy.r);

using Res_AccumPrevious = Vk::GraphImage<"AccumPrevious", Vk::Format::R16G16B16A16_SFloat, false, true>;
using Res_AccumCurrent  = Vk::GraphImage<"AccumCurrent", Vk::Format::R16G16B16A16_SFloat, false, true>;

} // namespace ZHLN

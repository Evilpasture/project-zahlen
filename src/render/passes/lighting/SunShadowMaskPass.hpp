// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "render/PassPayloads.hpp"
#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

// Cascade PCSS and the hardware sun contact ray, once per pixel, into
// Res_SunShadowMask (R8_UNORM). The lighting pass multiplies the mask in, so its
// fragment stage never runs the blocker search, the Poisson fan-out or the
// divergent BVH walk -- see sun_shadow_mask.slang for why that is the point, and
// what this pass is the seat for (a spatial denoiser over soft RT sun shadows).
//
// Usages: Res_SunShadowMask is a ComputeWrite, which the graph turns into a
// transition to GENERAL plus STORAGE_SHADER_WRITE -> SHADER_READ_ONLY before the
// lighting pass reads it as ShaderRead. The mask is full resolution, so it is a
// whole-image dependency of a full-screen pass, not a partitioned one.
struct SunShadowMaskPass:
    Vk::RenderPass<
        "SunShadowMask",
        Vk::FragmentAndComputeRead<Res_Depth>,
        Vk::FragmentAndComputeRead<Res_NormRough>,
        Vk::ShaderRead<Res_ShadowMap>,
        Vk::ComputeWrite<Res_SunShadowMask>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes

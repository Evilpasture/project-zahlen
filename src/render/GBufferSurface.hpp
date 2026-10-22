// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FrameContext.hpp"
#include "Rendering.hpp"
#include "TargetManager.hpp"

namespace ZHLN::Passes {

struct GBufferSurface {
    Vk::ImageSlice depth;
    Vk::ImageSlice normals;
};

struct OpaqueSurface {
    using Depth   = Res_Depth;
    using Normals = Res_NormRough;

    OpaqueSurface(const OpaqueSurface&)            = default;
    OpaqueSurface(OpaqueSurface&&)                 = default;
    OpaqueSurface& operator=(const OpaqueSurface&) = default;
    OpaqueSurface& operator=(OpaqueSurface&&)      = default;
    // Mirrors ScenePassPushConstants::translucentSurface for this G-buffer.
    static constexpr int kTranslucentSurface = 0;

    [[nodiscard]] static auto Resolve(RenderFrameContext& impl) noexcept -> GBufferSurface {
        return {
            .depth   = Vk::Assume<Vk::ShaderRead<Depth>>(impl.ActiveDepthTarget()).Raw(),
            .normals = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.normalRoughnessBuffer).Raw(),
        };
    }
};

struct TranslucentSurface {
    using Depth   = Res_TransDepth;
    using Normals = Res_TransNorm;

    // Mirrors ScenePassPushConstants::translucentSurface for this G-buffer.
    static constexpr int kTranslucentSurface = 1;

    [[nodiscard]] static auto Resolve(RenderFrameContext& impl) noexcept -> GBufferSurface {
        return {
            .depth   = Vk::Assume<Vk::ShaderRead<Depth>>(impl.graphResources.transDepthBuffer).Raw(),
            .normals = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.transNormalBuffer).Raw(),
        };
    }
};

} // namespace ZHLN::Passes

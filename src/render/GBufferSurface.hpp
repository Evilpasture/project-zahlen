// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderFrame.hpp"
#include "Rendering.hpp"
#include "TargetManager.hpp"
#include "graph/RenderPassDecl.hpp"

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

    [[nodiscard]] static auto Resolve(RenderFrame& frame) noexcept -> GBufferSurface {
        return {
            .depth   = Vk::Assume<Vk::ShaderRead<Depth>>(frame.ActiveDepthTarget()).Raw(),
            .normals = Vk::Assume<Vk::ShaderRead<Normals>>(frame.graphResources.normalRoughnessBuffer).Raw(),
        };
    }
};

struct TranslucentSurface {
    using Depth   = Res_TransDepth;
    using Normals = Res_TransNorm;

    // Mirrors ScenePassPushConstants::translucentSurface for this G-buffer.
    static constexpr int kTranslucentSurface = 1;

    [[nodiscard]] static auto Resolve(RenderFrame& frame) noexcept -> GBufferSurface {
        return {
            .depth   = Vk::Assume<Vk::ShaderRead<Depth>>(frame.graphResources.transDepthBuffer).Raw(),
            .normals = Vk::Assume<Vk::ShaderRead<Normals>>(frame.graphResources.transNormalBuffer).Raw(),
        };
    }
};

} // namespace ZHLN::Passes

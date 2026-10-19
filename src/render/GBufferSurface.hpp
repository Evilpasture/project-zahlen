// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"

namespace ZHLN::Passes {

struct GBufferSurface {
    Vk::ImageSlice depth;
    Vk::ImageSlice normals;
};

struct OpaqueSurface {
    using Depth      = Res_Depth;
    using Normals    = Res_NormRough;

    // Mirrors ScenePassPushConstants::translucentSurface for this G-buffer.
    static constexpr int kTranslucentSurface = 0;

    [[nodiscard]] static auto Resolve(RenderFrameContext& impl) noexcept -> GBufferSurface {
        return {
            .depth      = Vk::Assume<Vk::ShaderRead<Depth>>(impl.presenter.depthTarget).Raw(),
            .normals    = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.normalRoughnessBuffer).Raw(),
        };
    }
};

struct TranslucentSurface {
    using Depth      = Res_TransDepth;
    using Normals    = Res_TransNorm;

    // Mirrors ScenePassPushConstants::translucentSurface for this G-buffer.
    static constexpr int kTranslucentSurface = 1;

    [[nodiscard]] static auto Resolve(RenderFrameContext& impl) noexcept -> GBufferSurface {
        return {
            .depth      = Vk::Assume<Vk::ShaderRead<Depth>>(impl.graphResources.transDepthBuffer).Raw(),
            .normals    = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.transNormalBuffer).Raw(),
        };
    }
};

}

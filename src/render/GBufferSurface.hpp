// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"

namespace ZHLN::Passes {

// A resolved G-buffer surface contains the live views needed by raster passes.
// The resource tags stay on the archetype types below, where the render graph
// can use them for dependency and barrier analysis.
struct GBufferSurface {
    Vk::ImageSlice depth;
    Vk::ImageSlice normals;
    Vk::ImageSlice sheen;
    Vk::ImageSlice anisotropy;
};

struct OpaqueSurface {
    using Depth      = Res_Depth;
    using Normals    = Res_NormRough;
    using Sheen      = Res_Sheen;
    using Anisotropy = Res_Anisotropy;

    [[nodiscard]] static auto Resolve(RenderContext::Impl& impl) noexcept -> GBufferSurface {
        return {
            .depth      = Vk::Assume<Vk::ShaderRead<Depth>>(impl.presenter.depthTarget).Raw(),
            .normals    = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.normalRoughnessBuffer).Raw(),
            .sheen      = Vk::Assume<Vk::ShaderRead<Sheen>>(impl.graphResources.sheenBuffer).Raw(),
            .anisotropy = Vk::Assume<Vk::ShaderRead<Anisotropy>>(impl.graphResources.anisotropyBuffer).Raw(),
        };
    }
};

struct TranslucentSurface {
    using Depth      = Res_TransDepth;
    using Normals    = Res_TransNorm;
    using Sheen      = Res_TransSheen;
    using Anisotropy = Res_TransAnisotropy;

    [[nodiscard]] static auto Resolve(RenderContext::Impl& impl) noexcept -> GBufferSurface {
        return {
            .depth      = Vk::Assume<Vk::ShaderRead<Depth>>(impl.graphResources.transDepthBuffer).Raw(),
            .normals    = Vk::Assume<Vk::ShaderRead<Normals>>(impl.graphResources.transNormalBuffer).Raw(),
            .sheen      = Vk::Assume<Vk::ShaderRead<Sheen>>(impl.graphResources.transSheenBuffer).Raw(),
            .anisotropy = Vk::Assume<Vk::ShaderRead<Anisotropy>>(impl.graphResources.transAnisotropyBuffer).Raw(),
        };
    }
};

}

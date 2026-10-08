// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/RenderGraph.hpp"
#include "pipeline/FullscreenPass.hpp"
#include <GeneratedGpuTypes.hpp>
#include <Zahlen/Render/RenderContext.hpp>

namespace ZHLN::Passes {
struct GBufferSurface;
}

namespace ZHLN {

class ReflectionPipeline {
public:
    explicit ReflectionPipeline(RenderContext::Impl& impl) noexcept:
        _impl{impl} {
    }

    // Both graph passes use the same shader, variants, attachment format, and
    // descriptor declarations, so they share one fullscreen pipeline/binding set.
    Vk::FullscreenPass<Vk::ReflectedLayout> pass;

    void Dispatch(
        Vk::RasterPassContextBase& ctx,
        const Passes::GBufferSurface& surface,
        const GeneratedGpu::ScenePassPushConstants& pc
    ) const noexcept;

private:
    RenderContext::Impl& _impl;
};

}

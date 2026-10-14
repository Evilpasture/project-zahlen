// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/RenderGraph.hpp"
#include "pipeline/FullscreenPass.hpp"
#include <GeneratedGpuTypes.hpp>

namespace ZHLN::Passes {
struct GBufferSurface;
}

namespace ZHLN {

struct RenderFrameContext;

class ReflectionPipeline {
public:
    explicit ReflectionPipeline(RenderFrameContext& ctx) noexcept:
        _ctx{ctx} {
    }

    Vk::FullscreenPass<Vk::ReflectedLayout> pass;

    void Dispatch(
        Vk::RasterPassContextBase& ctx,
        const Passes::GBufferSurface& surface,
        const GeneratedGpu::ScenePassPushConstants& pc
    ) const noexcept;

private:
    RenderFrameContext& _ctx;
};

}

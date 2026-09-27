// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Forward-shaded geometry over the composited opaque image: forward-only
// (translucent) draws, billboard particles, and debug lines. It writes the HDR
// scene color and tests against the main depth buffer without writing it, so
// it can sit after the deferred chain without disturbing it.
struct ForwardPass: Vk::RenderPass<"Forward", Vk::ColorWrite<Res_HdrSceneColor>, Vk::DepthStencilWrite<Res_Depth>, Vk::ShaderRead<Res_TransLighting>> {
    RenderContext::Impl& impl;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}

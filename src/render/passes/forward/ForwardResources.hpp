// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/forward/ForwardResources.hpp
//
// The two pipelines the forward pass draws with, after the GBuffer has
// resolved: debug lines and billboard particles -- step 2 of
// rendercontext-impl-decomposition.md. One struct because ForwardPass is the
// only consumer of either.
//
// linePipeline is also read by Impl::FlushLineQueue, which is why this lives
// on Impl rather than inside the pass: the line queue is flushed from the draw
// command path, not from the pass.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ForwardResources {
    Vk::Pipeline                line;
    Vk::TypedPipeline<1, false> particles;
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/forward/ForwardResources.hpp
//
// The two pipelines the forward pass draws with, after the GBuffer has
// resolved: debug lines and billboard particles -- step 2 of
// rendercontext-impl-decomposition.md. One struct because ForwardPass is the
// only consumer of either.
//
// ForwardPass is the only reader of either pipeline. They still live on Impl
// rather than inside the pass because they are built once at init and outlive
// any one frame's pass instance, the same as the other *Resources groups.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ForwardResources {
    Vk::Pipeline                line;
    Vk::TypedPipeline<1, false> particles;
};

} // namespace ZHLN

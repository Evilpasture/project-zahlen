// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/gbuffer/DecalResources.hpp
//
// The decal pipeline and the descriptor layout it was built from -- step 2 of
// rendercontext-impl-decomposition.md. The layout is here rather than staying
// init-only because BuildDecalPipeline needs it again on a shader hot-reload.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct DecalResources {
    Vk::ReflectedLayout layout;
    Vk::Pipeline        pass;
};

} // namespace ZHLN

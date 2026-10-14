// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct CsgResources {
    Vk::Pipeline write;
    Vk::Pipeline difference;
    Vk::Pipeline intersection;
};

struct MeshParticlePipelines {
    Vk::Pipeline render;
    Vk::Pipeline shadow;
};

} // namespace ZHLN

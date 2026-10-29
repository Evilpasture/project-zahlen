// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "DrawCommands.hpp"
#include "Rendering.hpp"
#include <cstdint>

namespace ZHLN {

template <VkImageLayout ColorL, VkImageLayout DepthL>
struct SceneResources {
    Vk::TypedImage<ColorL> sceneColor;
    Vk::TypedImage<ColorL> velocity;
    Vk::TypedImage<ColorL> normRough;
    Vk::TypedImage<ColorL> bakedMask;
    Vk::TypedImage<DepthL> depth;
};

struct GroupRange {
    const NativeMaterial* material;
    uint32_t              start;
    uint32_t              count;
    VkCullModeFlags       cullMode;
};

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

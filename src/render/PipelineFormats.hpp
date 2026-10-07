// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

// Compile-time output signature used by the deferred G-buffer pass and the
// remaining runtime-built scene pipelines during migration.
using ActiveGBuffer = Vk::GBufferLayout<
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>,
    Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>,
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>,
    Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>,
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>,
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>,
    Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>>;

}

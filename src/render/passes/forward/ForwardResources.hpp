// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/Rendering.hpp"

namespace ZHLN {

struct ForwardResources {
    // Adds deferred lit material emission into LightingGI before reflection.
    Vk::TypedPipeline<1, true> emissive;
    Vk::Pipeline                line;
    Vk::TypedPipeline<1, false> particles;
};

} // namespace ZHLN

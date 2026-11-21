// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/Rendering.hpp"
#include "vulkan/pipeline/ComputePass.hpp"

namespace ZHLN {

struct ComputePassResources {
    Vk::DynamicComputePass skinning;
    Vk::DynamicComputePass proceduralBake;
    Vk::DynamicComputePass hangGpu;
};

} // namespace ZHLN

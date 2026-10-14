// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ComputePassResources {
    Vk::DynamicComputePass skinning;
    Vk::DynamicComputePass proceduralBake;
    Vk::DynamicComputePass hangGpu;
};

} // namespace ZHLN

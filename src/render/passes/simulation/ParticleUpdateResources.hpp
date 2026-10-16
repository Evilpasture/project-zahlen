// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "features/PostProcessFeature.hpp"

namespace ZHLN {

struct ParticleUpdateResources {
    Vk::DynamicComputePass particles {};
    Vk::DynamicComputePass mesh {};
};

} // namespace ZHLN

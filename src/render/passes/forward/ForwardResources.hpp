// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ForwardResources {
    Vk::Pipeline                line;
    Vk::TypedPipeline<1, false> particles;
};

} // namespace ZHLN

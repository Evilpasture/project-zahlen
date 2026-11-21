// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/Rendering.hpp"

namespace ZHLN {

struct DecalResources {
    Vk::ReflectedLayout layout;
    Vk::Pipeline        pass;
};

} // namespace ZHLN

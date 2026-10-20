// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../graph/GraphImage.hpp"

namespace ZHLN {
// The presentation depth image is also the depth resource of the frame DAG.
// TODO(Evilpasture): remove this
using Res_Depth = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT_S8_UINT>;
} // namespace ZHLN

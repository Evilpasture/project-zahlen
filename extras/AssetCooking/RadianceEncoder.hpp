// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Render/EnvironmentImage.hpp>
#include <cstddef>
#include <vector>

namespace ZHLN::AssetCooking {

// Offline-only ZRD1 writer. The engine consumes prepared float pixels, not
// this container or its reader/writer.
[[nodiscard]] auto EncodeCookedRadiance(const EnvironmentImage& image) -> std::vector<std::byte>;

} // namespace ZHLN::AssetCooking

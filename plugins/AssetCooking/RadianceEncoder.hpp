// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Render/EnvironmentImage.hpp>
#include <cstddef>
#include <vector>

namespace ZHLN::AssetCooking {

// Offline-only ZRD2 writer. It prepares raw pixels if necessary. The engine
// consumes prepared float pixels/metadata, not this container or its codec.
[[nodiscard]] auto EncodeCookedRadiance(const EnvironmentImage& image) -> std::vector<std::byte>;

} // namespace ZHLN::AssetCooking

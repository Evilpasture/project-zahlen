// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Hash.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN {

using AssetID    = uint64_t;
using MaterialID = uint64_t;

inline constexpr AssetID    InvalidAssetID    = 0;
inline constexpr MaterialID InvalidMaterialID = 0;

constexpr AssetID HashAssetID(std::string_view name) noexcept {
    return Hash64(name);
}

}

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/AssetID.hpp
//
// The two 64-bit identity types the whole engine addresses content by, and the
// hash that mints them. Nothing here knows what an asset is -- a cooked mesh, a
// texture, a material and a scene document are all just an id to the code that
// moves them around -- which is what keeps this header down to one dependency
// (Core/Hash.hpp) and lets the ECS, the cooker and the renderer agree on an
// identity without agreeing on a payload.
//
// Kept out of the renderer: a MaterialID names a runtime material the renderer
// owns, and an AssetID may name anything at all, so neither belongs to a
// subsystem that would then have to be included to spell an id.
#pragma once
#include <Zahlen/Core/Hash.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN {

// --- High-Level Persistent Asset Identifiers
using AssetID    = uint64_t;
using MaterialID = uint64_t;

inline constexpr AssetID    InvalidAssetID    = 0;
inline constexpr MaterialID InvalidMaterialID = 0;

constexpr AssetID HashAssetID(std::string_view name) noexcept {
    return Hash64(name);
}

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AssetCooking/CookError.hpp
//
// The bakers' shared error domain. Kept in its own header so a consumer of
// one baker (e.g. FontBake) does not drag in every other baker's headers.

#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN::AssetCooking {

enum class CookError : uint32_t {
    InvalidInput ZHLN_ANNOTATION(ZHLN::Description<"cooking input spans are empty, mis-sized, or inconsistent"> {}) = 1,
    InvalidFont ZHLN_ANNOTATION(ZHLN::Description<"font bytes could not be parsed as TrueType/OpenType"> {}),
};

} // namespace ZHLN::AssetCooking

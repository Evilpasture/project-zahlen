// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Float4.h>
#include <Zahlen/Error.hpp>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ZHLN::BSP {

enum class VMTError : uint8_t {
    InvalidSyntax = 1,
    UnexpectedToken,
    FileNotFound,
};

// Parsed Valve Material Type (.vmt) data representation
struct VMTMaterial {
    std::string shader;      // e.g. "LightmappedGeneric", "VertexLitGeneric", "UnlitGeneric"
    std::string baseTexture; // $basetexture value
    std::string bumpMap;     // $bumpmap or $normalmap value
    std::string surfaceProp; // $surfaceprop value

    bool  isTranslucent = false; // $translucent 1
    bool  isAlphaTest   = false; // $alphatest 1
    float alphaCutoff   = 0.5f;  // $alphatestreference
    bool  noCull        = false; // $nocull 1

    JPH::Float4 baseColor {1.0f, 1.0f, 1.0f, 1.0f}; // $color / $color2

    // All parameters preserved in lowercase (e.g. "$basetexture", "$surfaceprop")
    std::unordered_map<std::string, std::string> parameters;

    [[nodiscard]] auto Has(std::string_view key) const noexcept -> bool;
    [[nodiscard]] auto Get(std::string_view key, std::string_view defaultVal = "") const noexcept -> std::string_view;
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> std::optional<std::string_view>;
};

// Parses a VMT KeyValues script.
[[nodiscard]] auto ParseVMT(std::string_view text) -> std::expected<VMTMaterial, ErrorCode>;

} // namespace ZHLN::BSP

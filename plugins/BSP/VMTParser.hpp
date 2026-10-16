// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Physics/Jolt.hpp>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Error.hpp>
#include <expected>
#include <functional>
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

    // A `patch` material's overlay blocks, captured separately: "replace"
    // entries override the included material and "add" entries only fill keys
    // it does not define. Empty for ordinary materials.
    std::unordered_map<std::string, std::string> patchReplace;
    std::unordered_map<std::string, std::string> patchAdd;

    [[nodiscard]] auto Has(std::string_view key) const noexcept -> bool;
    [[nodiscard]] auto Get(std::string_view key, std::string_view defaultVal = "") const noexcept -> std::string_view;
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> std::optional<std::string_view>;
};

// Shared Source-material rules used by importing and lighting.
[[nodiscard]] auto ShaderEmissiveStrength(std::string_view shader) noexcept -> float;
[[nodiscard]] auto Rec709Luma(const JPH::Vec3& rgb) noexcept -> float;

// Parses a VMT KeyValues script. Nested shader-class blocks
// ("LightmappedGeneric_DX9 { ... }") are merged into parameters, because that
// is where Source materials keep their $bumpmap and friends; "proxies" and the
// legacy DX7/DX8 variants are ignored. Derives the material's fields from the
// final parameter set.
[[nodiscard]] auto ParseVMT(std::string_view text) -> std::expected<VMTMaterial, ErrorCode>;

// Loads a patch material's "include" target: given the path as written in the
// file, returns the .vmt text, or nullopt when the file is absent.
using VmtIncludeLoader = std::function<std::optional<std::string>(std::string_view)>;

// Parses a VMT and follows Source's `patch` include-plus-replace indirection
// (roughly a sixth of a shipped map's materials), so the result is the real
// material with the patch's own keys, then its "add", then its "replace"
// entries layered on top. Up to four indirections are followed, matching the
// loader's previous bound.
[[nodiscard]] auto ResolveVmtMaterial(std::string_view text, const VmtIncludeLoader& loadInclude) -> std::expected<VMTMaterial, ErrorCode>;

} // namespace ZHLN::BSP

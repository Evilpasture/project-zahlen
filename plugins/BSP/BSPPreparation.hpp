// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BSPGeometry.hpp"
#include "SourceVFS.hpp"
#include "VMTParser.hpp"
#include <optional>
#include <span>
#include <string_view>
#include <vector>
namespace ZHLN::BSP {
[[nodiscard]] auto LoadMaterialVmt(const SourceVFS& vfs, std::string_view materialName) -> std::optional<VMTMaterial>;
[[nodiscard]] auto RefineEmissiveSurfaces(const SourceVFS& vfs, std::span<EmissiveSurface> surfaces, ImportedMapData& data) -> std::vector<EmissiveSurface>;
void               ShiftImportedGeometry(ImportedMapData& data, FVec3 offset);
} // namespace ZHLN::BSP

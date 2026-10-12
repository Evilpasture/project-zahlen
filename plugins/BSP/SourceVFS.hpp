// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ZHLN::BSP {

// Virtual File System for resolving Source engine assets (materials, textures,
// models, lightmaps) across disk search paths and embedded BSP pakfiles.
// Handles case-insensitive lookups on POSIX filesystems.
class SourceVFS {
  public:
    SourceVFS() = default;

    // Adds a search directory on disk (e.g. "import/engine_assets", "csgo").
    void AddSearchPath(std::string_view path);

    // Sets raw BSP file bytes to parse embedded PAKFILE (Lump 40) zip archive.
    void SetPakfileData(std::span<const std::byte> pakData);

    // Resolves a virtual path (e.g. "materials/brick/brickwall003d.vmt")
    // to an actual disk path, case-insensitively.
    [[nodiscard]] auto ResolveFile(std::string_view virtualPath) const -> std::optional<std::string>;

    // Reads the entire contents of a file from disk or the embedded pakfile.
    [[nodiscard]] auto ReadFile(std::string_view virtualPath) const -> std::optional<std::vector<std::byte>>;

    // Resolves a material name (e.g. "brick/brickwall003d" or "materials/brick/brickwall003d")
    // to a .vmt file path.
    [[nodiscard]] auto ResolveMaterial(std::string_view materialName) const -> std::optional<std::string>;

    // Resolves a texture name to a .vtf, .png, or .jpg file path.
    [[nodiscard]] auto ResolveTexture(std::string_view textureName) const -> std::optional<std::string>;

    // Resolves a model name (e.g. "props_c17/furniturechair001a.mdl" or "models/props_c17/furniturechair001a.mdl")
    // to an actual disk path.
    [[nodiscard]] auto ResolveModel(std::string_view modelName) const -> std::optional<std::string>;

    // Resolves a lightmap atlas path for a map (e.g. "lightmaps/gm_murder_drones_lightmap0.png").
    [[nodiscard]] auto ResolveLightmap(std::string_view mapName) const -> std::optional<std::string>;

    // Checks whether an asset exists in any search path or pakfile.
    [[nodiscard]] auto Exists(std::string_view virtualPath) const -> bool;

    // Normalized canonical path helper (lowercase, forward slashes).
    [[nodiscard]] static auto NormalizePath(std::string_view path) -> std::string;

  private:
    struct PakEntry {
        uint32_t offset = 0;
        uint32_t compSize = 0;
        uint32_t uncompSize = 0;
        uint16_t method = 0; // 0 = store, 8 = deflate
    };

    void IndexDirectory(const std::filesystem::path& root);
    void IndexPakfile(std::span<const std::byte> pakData);

    std::vector<std::string> _searchPaths;
    // Map from normalized lowercase relative path -> actual disk path
    mutable std::unordered_map<std::string, std::string> _diskIndex;
    // Map from normalized lowercase path -> pakfile entry
    std::unordered_map<std::string, PakEntry> _pakIndex;
    std::vector<std::byte>                    _pakDataCopy;
    mutable bool                              _indexed = false;
};

} // namespace ZHLN::BSP

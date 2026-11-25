// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ZHLN::COM { struct IAssetLibrary; }
namespace ZHLN::FS { class VirtualFileSystem; }

namespace ZHLN::BSP {

// Virtual File System adapter for resolving Source engine materials, textures,
// and models across an embedded BSP pakfile (Lump 40) layered over a core VFS
// (`COM::IAssetLibrary` or `FS::VirtualFileSystem`).
//
// Never accesses the host OS filesystem directly: hosts mount Source game/mod
// directories into the engine VFS (with `caseInsensitive = true` when serving
// mixed-case Source assets on POSIX hosts) and optionally pass a `virtualPrefix`.
class SourceVFS {
  public:
    SourceVFS() = default;

    // Borrowed backing VFS: must outlive this SourceVFS (and its copies).
    // Embedded map Lump 40 pak data always takes precedence over the backing VFS.
    explicit SourceVFS(const COM::IAssetLibrary& assets, std::string_view virtualPrefix = {})
        : _assets(&assets), _virtualPrefix(NormalizePath(virtualPrefix)) {}

    explicit SourceVFS(const FS::VirtualFileSystem& vfs, std::string_view virtualPrefix = {})
        : _vfs(&vfs), _virtualPrefix(NormalizePath(virtualPrefix)) {}

    // Restricts backing-VFS lookups to paths under `virtualPrefix` (e.g. "source").
    void SetVirtualPrefix(std::string_view virtualPrefix);

    // Sets raw BSP Lump 40 zip bytes for embedded pakfile resolution.
    void SetPakfileData(std::span<const std::byte> pakData);

    // Resolves a virtual path (e.g. "materials/brick/brickwall003d.vmt")
    // to its normalized logical Source path if present in the pakfile or backing VFS.
    [[nodiscard]] auto ResolveFile(std::string_view virtualPath) const -> std::optional<std::string>;

    // Reads the entire contents of a file from the embedded pakfile or backing VFS.
    [[nodiscard]] auto ReadFile(std::string_view virtualPath) const -> std::optional<std::vector<std::byte>>;

    // Resolves a material name (e.g. "brick/brickwall003d" or "materials/brick/brickwall003d")
    // to a .vmt or .png virtual path.
    [[nodiscard]] auto ResolveMaterial(std::string_view materialName) const -> std::optional<std::string>;

    // Resolves a texture name to a .vtf, .png, or .jpg virtual path.
    [[nodiscard]] auto ResolveTexture(std::string_view textureName) const -> std::optional<std::string>;

    // Resolves a model name (e.g. "props_c17/furniturechair001a.mdl" or "models/props_c17/furniturechair001a.mdl")
    // to a virtual path.
    [[nodiscard]] auto ResolveModel(std::string_view modelName) const -> std::optional<std::string>;

    // Checks whether an asset exists in the embedded pakfile or backing VFS.
    [[nodiscard]] auto Exists(std::string_view virtualPath) const -> bool;

    // Normalized canonical path helper (lowercase, forward slashes).
    [[nodiscard]] static auto NormalizePath(std::string_view path) -> std::string;

  private:
    struct PakEntry {
        uint32_t offset     = 0;
        uint32_t compSize   = 0;
        uint32_t uncompSize = 0;
        uint16_t method     = 0; // 0 = store, 8 = deflate
    };

    [[nodiscard]] auto ApplyPrefix(std::string_view normalizedPath) const -> std::string;
    [[nodiscard]] auto BackingExists(std::string_view candidatePath) const -> bool;
    [[nodiscard]] auto BackingRead(std::string_view candidatePath) const -> std::optional<std::vector<std::byte>>;
    void               IndexPakfile(std::span<const std::byte> pakData);

    const COM::IAssetLibrary*    _assets = nullptr;
    const FS::VirtualFileSystem* _vfs    = nullptr;
    std::string                  _virtualPrefix;
    // Map from normalized lowercase path -> pakfile entry
    std::unordered_map<std::string, PakEntry> _pakIndex;
    std::vector<std::byte>                    _pakDataCopy;
};

} // namespace ZHLN::BSP

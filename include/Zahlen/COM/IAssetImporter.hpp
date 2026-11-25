// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable COM capability interface for dynamically loaded asset importer
// plugins (e.g. zahlen_plugin_gltf, zahlen_plugin_bsp). Format parsers and
// third-party decoders (cgltf, stb_image, simdjson, Source BSP/VTF/MDL) stay
// sealed inside the plugin .so while communicating with the host solely through
// IAssetLibrary and IGraphicsDevice.

#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::COM {

// Generic import options accepted via COM::InChain by IAssetImporter plugins.
// Format-specific plugins may also accept their own POD chain nodes alongside
// this struct.
struct AssetImportOptions {
    bool buildBLAS      = true;
    bool buildColliders = true;
};

static_assert(std::is_standard_layout_v<AssetImportOptions> && std::is_trivially_copyable_v<AssetImportOptions>);

struct IAssetImporter : IObject {
    // Short lowercase identifier for the format handled by this plugin
    // (e.g. "gltf", "bsp").
    [[nodiscard]] virtual StringView FormatName() const noexcept = 0;

    // Decode raw format bytes, upload GPU meshes/materials/textures through
    // `graphics`, register the prefab(s) under `virtualPath` in `assets`, and
    // return the primary prefab hash. Optional format/upload parameters arrive
    // via `options` (a COM::ChainInNode chain).
    [[nodiscard]] virtual Result<uint64_t> LoadFromBytes(
        IAssetLibrary*        assets,
        IGraphicsDevice*      graphics,
        Span<const std::byte> bytes,
        StringView            virtualPath,
        const ChainInNode*    options = nullptr
    ) noexcept = 0;

    // Read a format asset strictly from `virtualPath` (and any relative
    // dependencies) through `assets`'s VFS, upload GPU resources, register the
    // prefab(s) in `assets`, and return the primary prefab hash. Importers
    // must never touch the OS filesystem directly.
    [[nodiscard]] virtual Result<uint64_t> LoadFromVirtualPath(
        IAssetLibrary*     assets,
        IGraphicsDevice*   graphics,
        StringView         virtualPath,
        const ChainInNode* options = nullptr
    ) noexcept = 0;

    // Unload every TextureID created and owned by this importer instance. Call
    // only after despawning referencing entities and clearing cached prefabs.
    // Also invoked automatically from IAppLayer::OnStop when mounted in App.
    virtual void UnloadTextures(IGraphicsDevice* graphics) noexcept = 0;

  protected:
    ~IAssetImporter() = default;
};

static_assert(AuditInterfaceABI<IAssetImporter>(), "IAssetImporter violates the COM ABI rules.");

} // namespace ZHLN::COM

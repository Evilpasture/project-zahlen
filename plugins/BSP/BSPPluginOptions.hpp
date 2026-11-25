// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Fixed-layout COM::InChain option payload accepted by libzahlen_plugin_bsp's
// COM::IAssetImporter methods alongside COM::AssetImportOptions.

#include <Zahlen/Core/COM/Types.hpp>
#include <type_traits>

namespace ZHLN::BSP {

struct PluginImportOptions {
    bool            convertCoordinates   = true;
    float           unitScale            = 0.0254f; // Source inches to engine metres
    bool            includeDisplacements = true;
    bool            importSkybox         = false;
    bool            gatherLights         = true;
    bool            buildColliders       = true;
    COM::StringView assetRoot {};
};

static_assert(std::is_standard_layout_v<PluginImportOptions> && std::is_trivially_copyable_v<PluginImportOptions>);

} // namespace ZHLN::BSP

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Render/RenderIDs.hpp>
#include <cstdint>
#include <vector>
namespace ZHLN::BSP {
// Meshes transfer to the prefab cache at registration. Textures remain owned
// by the caller: keep this manifest until all prefab users are despawned and
// the prefab cache is cleared, then unload the handles through IGraphicsDevice.
// A cache hit acquires nothing and returns an empty manifest. No destructive
// RAII teardown can silently invalidate existing instances.
struct SourceAsset {
    uint64_t               hash = 0;
    std::vector<TextureID> textures;
};
} // namespace ZHLN::BSP

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <vector>
namespace ZHLN::BSP::AssetAuthoring {
// Before registration this scope owns all uploaded meshes/textures. A failed
// later material, collision or registration operation cannot leak GPU buffers.
// On success the cache takes meshes; the caller receives the texture manifest.
struct Upload {
    COM::IGraphicsDevice&  graphics;
    std::vector<Mesh>      meshes;
    std::vector<TextureID> textures;
    bool                   committed = false;
    ~Upload() {
        if (committed)
            return;
        for (const auto& mesh: meshes)
            graphics.DestroyMesh(mesh);
        for (const auto texture: textures)
            graphics.UnloadTexture(texture);
    }
};

} // namespace ZHLN::BSP::AssetAuthoring

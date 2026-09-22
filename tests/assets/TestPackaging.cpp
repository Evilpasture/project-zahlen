// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <expected>

struct PackagingTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> asset_id_and_path_hashing() {
            uint64_t hash1 = ZHLN::HashAssetPath("models/player.glb");
            uint64_t hash2 = ZHLN::HashAssetPath("models/player.glb");
            uint64_t hash3 = ZHLN::HashAssetPath("textures/albedo.png");

            ZHLN::Test::ExpectEq(hash1, hash2);
            ZHLN::Test::ExpectNe(hash1, hash3);

            ZHLN::AssetID id1 = ZHLN::HashAssetID("Mesh_LOD0");
            ZHLN::AssetID id2 = ZHLN::HashAssetID("Mesh_LOD0");
            ZHLN::Test::ExpectEq(id1, id2);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> binary_header_abi_packing() {
            // Packed binary disk layout ABI verification
            ZHLN::Test::ExpectEq(sizeof(ZHLN::FS::PakHeader), 20u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::FS::PakEntry), 36u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedTextureHeader), 28u);
            // v4 header: +12 bytes of VK_EXT_mesh_shader stream counts
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedMeshHeader), 56u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedAnimHeader), 20u);

            return {};
        }
    };
};

// Exported for the assets group binary (RunAssetsTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunPackagingSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<PackagingTestSuite>();
}


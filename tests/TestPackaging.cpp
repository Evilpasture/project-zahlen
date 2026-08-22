// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Types.hpp>
#include <expected>

struct PackagingTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> asset_id_and_path_hashing() {
            uint64_t hash1 = ZHLN::HashCreativeWorkPath("models/player.glb");
            uint64_t hash2 = ZHLN::HashCreativeWorkPath("models/player.glb");
            uint64_t hash3 = ZHLN::HashCreativeWorkPath("textures/albedo.png");

            ZHLN::Test::ExpectEq(hash1, hash2);
            ZHLN::Test::ExpectNe(hash1, hash3);

            ZHLN::AssetID id1 = ZHLN::HashAssetID("Mesh_LOD0");
            ZHLN::AssetID id2 = ZHLN::HashAssetID("Mesh_LOD0");
            ZHLN::Test::ExpectEq(id1, id2);

            return {};
        }

        std::expected<void, ZHLN::Error> binary_header_abi_packing() {
            // Packed binary disk layout ABI verification
            ZHLN::Test::ExpectEq(sizeof(ZHLN::PakHeader), 20u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::PakEntry), 36u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedTextureHeader), 28u);
            // v4 header: +12 bytes of VK_EXT_mesh_shader stream counts
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedMeshHeader), 56u);
            ZHLN::Test::ExpectEq(sizeof(ZHLN::CookedAnimHeader), 20u);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<PackagingTestSuite>();
}

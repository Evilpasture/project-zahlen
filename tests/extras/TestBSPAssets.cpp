// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include <BSP/StudioModelAssets.hpp>
#include <TestsFramework.hpp>
#include "helpers/StudioGraphicsProbe.hpp"
#include "helpers/StudioFixture.hpp"
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct SourceAssetDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("zahlen-com-studio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    SourceAssetDirectory() {
        std::filesystem::create_directories(path / "materials/studio");
        std::ofstream(path / "materials/studio/red.vmt") << "VertexLitGeneric { \"$basetexture\" \"studio/red\" \"$selfillum\" \"1\" }";
        const std::array<uint8_t, 70> png {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,248,207,192,240,31,0,5,0,1,255,137,153,61,29,0,0,0,0,73,69,78,68,174,66,96,130};
        std::ofstream image(path / "materials/studio/red.png", std::ios::binary);
        image.write(reinterpret_cast<const char*>(png.data()), png.size());
        std::ofstream(path / "empty.bin", std::ios::binary);
    }
    ~SourceAssetDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

auto Triangle() -> ZHLN::BSP::StudioModelData {
    ZHLN::BSP::StudioModelData data;
    ZHLN::BSP::StudioMeshPart part;
    part.materialName = "studio/red";
    part.positions = {{{-1, 0, -1}}, {{1, 0, -1}}, {{0, 0, 1}}};
    part.tangentFrames.resize(3); part.surfaces.resize(3);
    part.indices = {0, 1, 2};
    part.boundsMin = {-1, 0, -1}; part.boundsMax = {1, 0, 1};
    data.parts.push_back(std::move(part));
    return data;
}

struct BSPAssetsSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> source_model_files_import_through_capability_vfs() {
            using namespace ZHLN;
            using Test::ExpectTrue;
            using Test::ExpectEq;
            auto created = COM::CreateEngine(COM::InChain(
                COM::AppConfig {.appName = String64("COM Studio files"), .logLevel = LogLevel::Quiet},
                COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                COM::RenderConfig {.validationMode = ValidationMode::Off}));
            if (!ExpectTrue(created.has_value())) return {};
            auto engine = std::move(*created);
            auto& assets = *engine->Assets();
            auto& graphics = *engine->Graphics();
            SourceAssetDirectory directory;
            std::filesystem::create_directories(directory.path / "models");
            std::array<BSP::StudioVertex, 3> vertices {};
            vertices[1].pos[0] = 1; vertices[2].pos[1] = 1;
            for (auto& vertex: vertices) vertex.normal[2] = 1;
            std::array<BSP::VtxVertex, 3> remap {};
            remap[1].origMeshVertID = 1; remap[2].origMeshVertID = 2;
            const std::array<uint16_t, 3> indices {0, 1, 2};
            const auto mdl = Test::StudioFixture::BuildSyntheticMDL(1234, "models/real.mdl", {"studio/"}, {"red"});
            const auto vvd = Test::StudioFixture::BuildSyntheticVVD(1234, vertices);
            const auto vtx = Test::StudioFixture::BuildSyntheticVTX(1234, remap, indices);
            const auto write = [&](const char* file, const std::vector<std::byte>& bytes) {
                std::ofstream out(directory.path / file, std::ios::binary);
                out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            };
            write("models/real.mdl", mdl); write("models/real.vvd", vvd); write("models/real.vtx", vtx);
            const auto root = directory.path.string();
            if (!ExpectTrue(assets.MountDirectory(std::string_view(root)).HasValue())) return {};
            BSP::SourceVFS vfs(assets);
            const BSP::StudioAssetOptions options {.buildBLAS = false};
            auto disk = BSP::LoadStudioAsset(assets, graphics, vfs, "models/real.mdl", options);
            auto memory = BSP::LoadStudioAssetFromMemory(assets, graphics, vfs, mdl, vvd, vtx, {}, "models/memory.mdl", options);
            ExpectTrue(disk.has_value()); ExpectTrue(memory.has_value());
            for (const auto* imported: {&disk, &memory}) {
                if (!*imported) continue;
                // A malformed-file fallback has no textures. This assertion
                // proves that both entry points reached the real mesh/material
                // pipeline, not merely the placeholder policy.
                ExpectEq((*imported)->textures.size(), size_t {1});
                const auto info = assets.DescribePrefab((*imported)->hash);
                ExpectTrue(info.HasValue() && info.value.partCount == 1);
                auto instances = assets.Instantiate((*imported)->hash);
                ExpectTrue(!instances.empty());
                for (const auto entity: instances) engine->Entities()->Despawn(entity);
            }
            assets.ClearCache();
            if (disk) for (const auto texture: disk->textures) graphics.UnloadTexture(texture);
            if (memory) for (const auto texture: memory->textures) graphics.UnloadTexture(texture);
            ExpectTrue(assets.UnmountDirectory(std::string_view(root)).HasValue());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> capability_source_assets_upload_rollback_and_physics() {
            using namespace ZHLN;
            using Test::ExpectTrue;
            using Test::ExpectEq;
            auto created = COM::CreateEngine(COM::InChain(
                COM::AppConfig {.appName = String64("COM Source assets"), .logLevel = LogLevel::Quiet},
                COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                COM::RenderConfig {.validationMode = ValidationMode::Off}));
            if (!ExpectTrue(created.has_value())) return {};
            auto engine = std::move(*created);
            auto& assets = *engine->Assets();
            COM::StudioGraphicsProbe graphics(*engine->Graphics());
            SourceAssetDirectory directory;
            const auto root = directory.path.string();
            const auto mounted = assets.MountDirectory(std::string_view(root));
            if (!ExpectTrue(mounted.HasValue())) return {};
            BSP::SourceVFS vfs(assets);
            // No disk search path is installed: these paths resolve through
            // IAssetLibrary, including material includes and texture bytes.
            ExpectTrue(vfs.Exists("MATERIALS\\STUDIO\\RED.VMT"));
            const auto bytes = vfs.ReadFile("materials/studio/red.png");
            ExpectTrue(bytes.has_value() && !bytes->empty());
            const auto empty = vfs.ReadFile("empty.bin");
            ExpectTrue(empty.has_value() && empty->empty());
            ExpectTrue(!vfs.ReadFile("missing.bin").has_value());

            auto data = Triangle();
            const BSP::StudioAssetOptions raster {.import = {.buildColliders = true}, .buildBLAS = false};
            // Invalid complete input fails before the first allocation.
            data.parts[0].indices[2] = 99;
            auto bad = BSP::UploadStudioAsset(assets, graphics, vfs, data, "studio/bad", raster);
            ExpectTrue(!bad.has_value());
            ExpectEq(graphics.createdMeshes, 0u);
            data.parts[0].indices[2] = 2;
            ExpectTrue(!BSP::UploadStudioAsset(assets, graphics, vfs, data, "", raster).has_value());

            // Fail after mesh AND texture upload; exact typed error propagates,
            // unpublished allocations are retired and the prefab is absent.
            graphics.failMaterial = true;
            auto failed = BSP::UploadStudioAsset(assets, graphics, vfs, data, "studio/rollback", raster);
            ExpectTrue(!failed.has_value());
            if (!failed) ExpectTrue(failed.error() == COM::Error::InvalidArgument);
            ExpectEq(graphics.createdMeshes, 1u);
            ExpectEq(graphics.destroyedMeshes, 1u);
            ExpectEq(graphics.createdTextures, 1u);
            ExpectEq(graphics.unloadedTextures, 1u);
            ExpectTrue(!assets.HasPrefab(assets.PrefabHash("studio/rollback")));
            graphics.failMaterial = false;

            auto uploaded = BSP::UploadStudioAsset(assets, graphics, vfs, data, "studio/triangle", raster);
            if (!ExpectTrue(uploaded.has_value())) return {};
            ExpectEq(uploaded->textures.size(), size_t {1});
            const auto info = assets.DescribePrefab(uploaded->hash);
            ExpectTrue(info.HasValue());
            if (info.HasValue()) { ExpectEq(info.value.partCount, 1u); ExpectEq(info.value.nodeCount, 1u); }
            const auto meshCount = graphics.createdMeshes;
            auto cached = BSP::UploadStudioAsset(assets, graphics, vfs, data, "studio/triangle", raster);
            ExpectTrue(cached.has_value() && cached->hash == uploaded->hash && cached->textures.empty());
            ExpectEq(graphics.createdMeshes, meshCount);

            // Both collider selection modes retain the original Studio box,
            // including the mesh/static choice (not triangulated substitutes).
            for (const bool box: {false, true}) {
                auto instances = assets.Instantiate(uploaded->hash, PrefabFactory::SpawnParams {
                    .position = {box ? 4.0 : 0.0, 0, 0}, .createPhysics = true, .useBoxColliders = box});
                ExpectTrue(!instances.empty());
                bool body = false;
                for (const auto entity: instances) {
                    const auto* physics = engine->Entities()->Get<Components::PhysicsComponent>(entity);
                    if (physics != nullptr) body |= engine->Simulation()->BodyPosition(physics->physicsID).HasValue();
                }
                ExpectTrue(body);
                for (const auto entity: instances) engine->Entities()->Despawn(entity);
            }
            const auto mesh = engine->Graphics()->FindMesh(assets.AssetHash("studio/triangle#part_0"));
            ExpectTrue(mesh.HasValue());
            if (mesh.HasValue()) { ExpectEq(mesh.value.vertexCount, 3u); ExpectEq(mesh.value.indexCount, 3u); ExpectTrue(mesh.value.meshletCount > 0); }
            auto placeholder = BSP::LoadStudioAssetFromMemory(assets, graphics, vfs, {}, {}, {}, {}, "studio/placeholder", raster);
            ExpectTrue(placeholder.has_value());
            if (placeholder) {
                auto description = assets.DescribePrefab(placeholder->hash);
                ExpectTrue(description.HasValue() && description.value.partCount == 1);
            }
            auto missing = BSP::LoadStudioAsset(assets, graphics, vfs, "models/not-present.mdl", raster);
            ExpectTrue(missing.has_value());

            // Reject contradictory collider policy at the public boundary.
            const std::array<COM::PrefabPartInfo, 1> invalidParts {{{.useBoxForMeshCollider = true}}};
            auto invalid = assets.RegisterPrefab(COM::InChain(COM::PrefabRegistrationInfo {
                .virtualPath = "studio/invalid-collider", .parts = invalidParts}));
            ExpectTrue(!invalid.HasValue());
            ExpectTrue(!assets.HasPrefab(assets.PrefabHash("studio/invalid-collider")));

            assets.ClearCache();
            for (const auto texture: uploaded->textures) engine->Graphics()->UnloadTexture(texture);
            const auto unmounted = assets.UnmountDirectory(std::string_view(root));
            ExpectTrue(unmounted.HasValue());
            return {};
        }
    };
};
} // namespace

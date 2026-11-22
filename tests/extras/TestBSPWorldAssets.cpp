// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helpers/StudioGraphicsProbe.hpp"
#include <BSP/BSPAssets.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Render/Types.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
auto WorldMap() -> ZHLN::BSP::Map {
    using namespace ZHLN::BSP;
    Map map;
    map.version = 20;
    DPlane plane {};
    plane.normal[2] = 1;
    map.planes.push_back(plane);
    DTexInfo info {};
    info.textureVecs[0][0]  = 1;
    info.textureVecs[1][1]  = 1;
    info.lightmapVecs[0][0] = 1;
    info.lightmapVecs[1][1] = 1;
    map.texInfos.push_back(info);
    DTexData texture {};
    texture.width = texture.height = 16;
    texture.reflectivity[0] = texture.reflectivity[1] = texture.reflectivity[2] = 0.5f;
    map.texDatas.push_back(texture);
    map.texNames.push_back("source/lit");
    for (const float x: {0.0f, 100.0f}) {
        const auto base = static_cast<uint16_t>(map.vertices.size());
        map.vertices.push_back({{x - 1, -1, 0}});
        map.vertices.push_back({{x + 1, -1, 0}});
        map.vertices.push_back({{x + 1, 1, 0}});
        map.vertices.push_back({{x - 1, 1, 0}});
        for (uint16_t i = 0; i < 4; ++i) {
            map.edges.push_back({{static_cast<uint16_t>(base + i), static_cast<uint16_t>(base + ((i + 1) % 4))}});
            map.surfEdges.push_back(static_cast<int32_t>(map.edges.size() - 1));
        }
        DFace face {};
        face.firstedge = static_cast<int32_t>(map.surfEdges.size() - 4);
        face.numedges  = 4;
        face.dispinfo  = -1;
        face.lightofs  = -1;
        face.origFace  = -1;
        map.faces.push_back(face);
    }
    DModel world {};
    world.firstface = 0;
    world.numfaces  = 1;
    world.mins[0] = world.mins[1] = -1;
    world.maxs[0] = world.maxs[1] = 1;
    map.models.push_back(world);
    DModel door    = world;
    door.firstface = 1;
    door.mins[0]   = 99;
    door.maxs[0]   = 101;
    map.models.push_back(door);
    map.entities.push_back({.keys = {{"classname", "worldspawn"}}});
    map.entities.push_back({.keys = {{"classname", "func_door"}, {"model", "*1"}, {"origin", "100 0 0"}}});
    map.staticProps.emplace_back();
    map.staticProps.back().modelName = "models/source/prop.mdl";
    map.entities.push_back({.keys = {{"classname", "prop_dynamic"}, {"model", "models/source/prop.mdl"}}});
    return map;
}

auto StoredMapPak() -> std::vector<std::byte> {
    const std::string name     = "materials/source/lit.vmt";
    const std::string contents = "LightmappedGeneric { \"$color\" \"[0.1 0.2 0.3]\" }";
    uint32_t          crc      = 0xffffffffu;
    for (const auto c: contents) {
        crc ^= static_cast<uint8_t>(c);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    std::vector<std::byte> bytes;
    const auto             put = [&](uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i)
            bytes.push_back(std::byte((value >> (i * 8)) & 255));
    };
    const auto text = [&](const std::string& value) {
        for (const auto c: value)
            bytes.push_back(std::byte(c));
    };
    put(0x04034b50, 4);
    put(20, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(crc, 4);
    put(static_cast<uint32_t>(contents.size()), 4);
    put(static_cast<uint32_t>(contents.size()), 4);
    put(static_cast<uint32_t>(name.size()), 2);
    put(0, 2);
    text(name);
    text(contents);
    const auto directory = static_cast<uint32_t>(bytes.size());
    put(0x02014b50, 4);
    put(20, 2);
    put(20, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(crc, 4);
    put(static_cast<uint32_t>(contents.size()), 4);
    put(static_cast<uint32_t>(contents.size()), 4);
    put(static_cast<uint32_t>(name.size()), 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 2);
    put(0, 4);
    put(0, 4);
    text(name);
    const auto directorySize = static_cast<uint32_t>(bytes.size()) - directory;
    put(0x06054b50, 4);
    put(0, 2);
    put(0, 2);
    put(1, 2);
    put(1, 2);
    put(directorySize, 4);
    put(directory, 4);
    put(0, 2);
    return bytes;
}

auto WorldBytes(const ZHLN::BSP::Map& map) -> std::vector<std::byte> {
    using namespace ZHLN::BSP;
    Header header {};
    header.ident   = kBspIdent;
    header.version = 20;
    std::vector<std::byte> bytes(sizeof(header));
    const auto             add = [&](Lump lump, auto span) {
        auto  data    = std::as_bytes(span);
        auto& entry   = header.lumps[static_cast<size_t>(lump)];
        entry.fileofs = static_cast<int32_t>(bytes.size());
        entry.filelen = static_cast<int32_t>(data.size());
        bytes.insert(bytes.end(), data.begin(), data.end());
    };
    add(Lump::Planes, std::span(map.planes));
    add(Lump::Vertexes, std::span(map.vertices));
    add(Lump::Edges, std::span(map.edges));
    add(Lump::SurfEdges, std::span(map.surfEdges));
    add(Lump::Faces, std::span(map.faces));
    add(Lump::TexInfo, std::span(map.texInfos));
    add(Lump::TexData, std::span(map.texDatas));
    add(Lump::Models, std::span(map.models));
    const std::array<int32_t, 1> offsets {0};
    add(Lump::TexDataStringTable, std::span(offsets));
    const char name[] = "source/lit";
    add(Lump::TexDataStringData, std::span(name));
    const char entities[] = "{\"classname\" \"worldspawn\"}\n{\"classname\" \"func_door\" \"model\" \"*1\" \"origin\" \"100 0 0\"}";
    add(Lump::Entities, std::span(entities));
    const auto pak = StoredMapPak();
    add(Lump::PakFile, std::span(pak));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

auto WorldPart(float x, const char* material) -> ZHLN::BSP::MaterialStreams {
    ZHLN::BSP::MaterialStreams part;
    part.materialName = material;
    part.positions    = {{{x - 1, 0, -1}}, {{x, 0, 1}}, {{x + 1, 0, -1}}};
    part.tangentFrames.resize(3);
    part.surfaces.resize(3);
    part.indices      = {0, 1, 2};
    part.boundsMin    = {x - 1, 0, -1};
    part.boundsMax    = {x + 1, 0, 1};
    part.lightmapPage = 0;
    return part;
}

struct BSPWorldAssetsSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> world_and_moving_brush_documents_are_host_owned() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto               map = WorldMap();
            BSP::ImportOptions options {.convertCoordinates = false, .unitScale = 1};
            options.gatherEmissiveSurfaces = false;
            options.enforceLightingFloor   = false;
            auto prepared                  = BSP::PrepareAssets(map, {}, "maps/test.bsp", options);
            if (!ExpectTrue(prepared.has_value()))
                return {};
            ExpectEq(prepared->prefabs.size(), size_t {2});
            ExpectEq(prepared->studioModels.size(), size_t {1});
            if (!prepared->studioModels.empty())
                ExpectEq(prepared->studioModels[0], "models/source/prop.mdl");
            if (prepared->prefabs.size() != 2)
                return {};
            ExpectEq(prepared->prefabs[0].virtualPath, "maps/test.bsp");
            ExpectEq(prepared->prefabs[1].virtualPath, "maps/test.bsp#model_1");
            ExpectTrue(prepared->prefabs[0].geometry.boundsMax.x < 2);
            ExpectTrue(prepared->prefabs[1].geometry.boundsMin.x > -2 && prepared->prefabs[1].geometry.boundsMax.x < 2);
            ExpectTrue(prepared->prefabs[0].geometry.lights.empty());
            ExpectTrue(prepared->prefabs[1].geometry.lightmaps.pages.empty());
            auto bytes   = WorldBytes(map);
            auto decoded = BSP::DecodeAssets(bytes, {}, "maps/test.bsp", options);
            if (ExpectTrue(decoded.has_value())) {
                bytes.clear();
                map = {};
                ExpectEq(decoded->prefabs.size(), size_t {2});
                ExpectTrue(!decoded->prefabs[0].geometry.parts[0].positions.empty());
                const auto material = decoded->source.ReadFile("materials/source/lit.vmt");
                ExpectTrue(material.has_value());
                if (material) {
                    const std::string text(reinterpret_cast<const char*>(material->data()), material->size());
                    ExpectTrue(text.find("0.1 0.2 0.3") != std::string::npos);
                }
            }
            ExpectTrue(!BSP::DecodeAssets({}, {}, "broken.bsp", options).has_value());
            ExpectTrue(!BSP::ReadAssets({}, "not-present.bsp", options).has_value());
            ExpectTrue(!BSP::PrepareAssets(WorldMap(), {}, "", options).has_value());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> world_lightmaps_layers_compound_collisions_and_rollback() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = COM::CreateEngineRef(
                COM::InChain(
                    COM::AppConfig {.appName = String64("COM BSP world"), .logLevel = LogLevel::Quiet},
                    COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                    COM::RenderConfig {.validationMode = ValidationMode::Off}
                )
            );
            if (!ExpectTrue(created.has_value()))
                return {};
            auto                     engine = std::move(*created);
            auto&                    assets = *engine->Assets();
            COM::StudioGraphicsProbe graphics(*engine->Graphics());
            const auto               root = std::filesystem::temp_directory_path() /
                                            ("zahlen-bsp-world-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root / "materials/source");
            std::ofstream(root / "materials/source/lit.vmt") << "LightmappedGeneric {}";
            std::ofstream(root / "materials/source/glass.vmt") << "VertexLitGeneric {\"$translucent\" \"1\"}";
            BSP::SourceVFS source;
            source.AddSearchPath(root.string());
            BSP::AssetDocument document {.virtualPath = "world/test"};
            auto                  sky       = WorldPart(-10, "source/lit");
            sky.skyboxLayer                 = true;
            document.geometry.parts         = {sky, WorldPart(0, "source/lit"), WorldPart(10, "source/glass")};
            document.geometry.collisionOnly = WorldPart(4, "tools/toolsclip");
            document.geometry.lightmaps.pages.push_back({.width = 1, .height = 1, .rgba16f = {0x3c00, 0x3c00, 0x3c00, 0x3c00}});
            const BSP::AssetOptions raster {.buildBLAS = false};
            auto                       malformed   = document;
            malformed.geometry.parts[0].indices[0] = 100;
            ExpectTrue(!BSP::UploadAsset(assets, graphics, source, malformed, raster).has_value());
            ExpectEq(graphics.createdMeshes, 0u);
            ExpectEq(graphics.createdTextures, 0u);

            graphics.failMaterial = true;
            auto failed           = BSP::UploadAsset(assets, graphics, source, document, raster);
            ExpectTrue(!failed.has_value());
            if (!failed)
                ExpectTrue(failed.error() == COM::Error::InvalidArgument);
            ExpectEq(graphics.createdMeshes, 1u);
            ExpectEq(graphics.destroyedMeshes, 1u);
            ExpectEq(graphics.createdTextures, 1u);
            ExpectEq(graphics.unloadedTextures, 1u);
            ExpectTrue(!assets.HasPrefab(assets.PrefabHash("world/test")));
            graphics.failMaterial = false;
            auto imported         = BSP::UploadAsset(assets, graphics, source, document, raster);
            if (!ExpectTrue(imported.has_value())) {
                std::filesystem::remove_all(root);
                return {};
            }
            ExpectEq(imported->textures.size(), size_t {1});
            const auto count  = graphics.createdMeshes;
            auto       cached = BSP::UploadAsset(assets, graphics, source, document, raster);
            ExpectTrue(cached.has_value() && cached->textures.empty());
            ExpectEq(graphics.createdMeshes, count);
            // Registration copied the documents and collision data.
            document.geometry = {};
            for (const bool box: {false, true}) {
                const double offset = box ? 20.0 : 0.0;
                const auto   entities =
                    assets.Instantiate(imported->hash, PrefabFactory::SpawnParams {.position = {offset, 0, 0}, .createPhysics = true, .useBoxColliders = box});
                ExpectTrue(!entities.empty());
                uint32_t bodies = 0;
                for (const auto entity: entities) {
                    if (engine->Entities()->Get<Components::PhysicsComponent>(entity))
                        ++bodies;
                }
                ExpectEq(bodies, 2u); // background skybox has no physics
                const auto toolHit = engine->Simulation()->Raycast({offset + 4, 5, 0}, {0, -1, 0});
                ExpectEq(toolHit.hasHit, !box); // tool child belongs ONLY to mesh choice
                const auto lit = engine->Graphics()->FindMaterial(assets.AssetHash("world/test#part_1_mat"));
                ExpectTrue(lit.HasValue());
                if (lit.HasValue())
                    ExpectTrue(lit.value.bakedIrradianceMap != TextureID::Invalid);
                const auto glass = engine->Graphics()->FindMaterial(assets.AssetHash("world/test#part_2_mat"));
                ExpectTrue(glass.HasValue());
                if (glass.HasValue())
                    ExpectEq(glass.value.bakedIrradianceMap, TextureID::Invalid);
                for (const auto entity: entities)
                    engine->Entities()->Despawn(entity);
            }
            const std::array<COM::PrefabPartInfo, 1> noBase {{{.additionalMeshCollision = HashCollisionAssetID("not-cached")}}};
            const auto invalid = assets.RegisterPrefab(COM::InChain(COM::PrefabRegistrationInfo {.virtualPath = "invalid-compound", .parts = noBase}));
            ExpectTrue(!invalid.HasValue());
            assets.ClearCache();
            for (const auto texture: imported->textures)
                engine->Graphics()->UnloadTexture(texture);
            std::filesystem::remove_all(root);
            return {};
        }
    };
};
} // namespace

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helpers/GLTFFixture.hpp"
#include "helpers/StudioGraphicsProbe.hpp"
#include <TestsFramework.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glTF/GLTFAssets.hpp>
namespace {
auto Fixture(std::string_view path) {
    const auto text = ZHLN::Test::GLTFFixture;
    return ZHLN::GLTF::Decode({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, path);
}
struct GLTFAssetsSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> owns_decoded_skin_animation_morph_material_and_image() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto doc = Fixture("fixture.gltf");
            if (!ExpectTrue(doc.has_value()))
                return {};
            ExpectEq(doc->parts.size(), size_t(1));
            ExpectEq(doc->primitives.size(), size_t(1));
            ExpectEq(doc->skeletons.size(), size_t(1));
            ExpectEq(doc->animations.size(), size_t(1));
            ExpectEq(doc->animations[0].channels.size(), size_t(2));
            ExpectEq(doc->primitives[0].activeMorphCount, 1u);
            ExpectEq(doc->primitives[0].morphDeltas.size(), size_t(12));
            ExpectEq(doc->primitives[0].skins.size(), size_t(3));
            ExpectEq(doc->parts[0].defaultMorphWeights.x, .75f);
            ExpectEq(doc->textures[0].rgba.size(), size_t(4));
            ExpectEq(doc->primitives[0].material.clearcoatFactor, .8f);
            ExpectEq(doc->primitives[0].material.emissive.x, 20.f);
            const std::array<uint8_t, 3> invalid {1, 2, 3};
            ExpectTrue(!GLTF::Decode(invalid, "bad.glb").has_value());
            return {};
        }
        std::expected<void, ZHLN::ErrorCode> imports_real_rig_and_its_animation_through_capabilities() {
            using namespace ZHLN;
            using Test::ExpectTrue;
            auto doc = GLTF::Read("resources/assets/ProceduralAnimationBaseRig.glb");
            if (!ExpectTrue(doc.has_value()))
                return {};
            ExpectTrue(!doc->parts.empty() && !doc->skeletons.empty() && !doc->animations.empty());
            auto created = COM::CreateEngineRef(
                COM::InChain(
                    COM::EngineAppConfig {.appName = String64("COM glTF rig"), .logLevel = LogLevel::Quiet},
                    COM::EngineWindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                    COM::EngineRenderConfig {.validationMode = ValidationMode::Off}
                )
            );
            if (!ExpectTrue(created.has_value()))
                return {};
            auto engine   = std::move(*created);
            auto uploaded = GLTF::Upload(*engine->Assets(), *engine->Graphics(), *doc, {.buildBLAS = false});
            if (!ExpectTrue(uploaded.has_value()))
                return {};
            auto entities = engine->Assets()->Instantiate(uploaded->hash);
            ExpectTrue(!entities.empty());
            for (const auto entity: entities)
                engine->Entities()->Despawn(entity);
            engine->Assets()->ClearCache();
            for (const auto texture: uploaded->textures)
                engine->Graphics()->UnloadTexture(texture);
            return {};
        }
        std::expected<void, ZHLN::ErrorCode> uploads_and_instantiates_without_native_importer_and_rolls_back_failure() {
            using namespace ZHLN;
            using Test::ExpectEq;
            using Test::ExpectTrue;
            auto created = COM::CreateEngineRef(
                COM::InChain(
                    COM::EngineAppConfig {.appName = String64("COM glTF"), .logLevel = LogLevel::Quiet},
                    COM::EngineWindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                    COM::EngineRenderConfig {.validationMode = ValidationMode::Off}
                )
            );
            if (!ExpectTrue(created.has_value()))
                return {};
            auto                     engine = std::move(*created);
            auto&                    assets = *engine->Assets();
            COM::StudioGraphicsProbe probe(*engine->Graphics());
            auto                     doc = Fixture("fixture-upload.gltf");
            if (!ExpectTrue(doc.has_value()))
                return {};
            probe.failMaterial = true;
            auto failed        = GLTF::Upload(assets, probe, *doc, {.buildBLAS = false});
            ExpectTrue(!failed.has_value());
            ExpectTrue(!assets.HasPrefab(assets.PrefabHash(std::string_view(doc->virtualPath))));
            ExpectEq(probe.createdMeshes, probe.destroyedMeshes);
            ExpectEq(probe.createdTextures, probe.unloadedTextures);
            probe.failMaterial = false;
            auto uploaded      = GLTF::Upload(assets, probe, *doc, {.buildBLAS = false});
            if (!ExpectTrue(uploaded.has_value()))
                return {};
            auto info = assets.DescribePrefab(uploaded->hash);
            if (!ExpectTrue(info.HasValue()))
                return {};
            ExpectEq(info.value.skeletonCount, 1u);
            ExpectEq(info.value.animationCount, 1u);
            ExpectEq(info.value.partCount, 1u);
            auto entities = assets.Instantiate(uploaded->hash);
            if (ExpectTrue(!entities.empty()))
                for (const auto entity: entities)
                    engine->Entities()->Despawn(entity);
            const auto root = std::filesystem::temp_directory_path() /
                              ("zahlen-gltf-vfs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(root / "models");
            {
                std::ofstream f(root / "models/model.gltf");
                f << Test::GLTFSplitFixture;
            }
            {
                std::ofstream f(root / "models/geometry.bin", std::ios::binary);
                f.write(reinterpret_cast<const char*>(Test::GLTFBinary.data()), Test::GLTFBinary.size());
            }
            {
                std::ofstream f(root / "models/pixel.png", std::ios::binary);
                f.write(reinterpret_cast<const char*>(Test::GLTFPNG.data()), Test::GLTFPNG.size());
            }
            ExpectTrue(assets.MountDirectory(std::string_view(root.string())).HasValue());
            auto split = GLTF::Read("models/model.gltf", {}, &assets);
            if (ExpectTrue(split.has_value())) {
                ExpectEq(split->primitives.size(), doc->primitives.size());
                ExpectEq(split->textures[0].rgba.size(), size_t(4));
            }
            ExpectTrue(assets.UnmountDirectory(std::string_view(root.string())).HasValue());
            std::filesystem::remove_all(root);
            auto conflict                        = *doc;
            conflict.options.emissiveFactorScale = 1;
            ExpectTrue(!GLTF::Upload(assets, probe, conflict, {.buildBLAS = false}).has_value());
            const auto count = probe.createdMeshes;
            auto       hit   = GLTF::Upload(assets, probe, *doc, {.buildBLAS = false});
            ExpectTrue(hit.has_value() && hit->textures.empty());
            ExpectEq(probe.createdMeshes, count);
            const std::array<COM::PrefabNodeInfo, 2> nodes {{{.name = String64("parent")}, {.name = String64("child"), .parentIndex = 0}}};
            std::array<COM::PrefabJointInfo, 2>      joints {
                {{.name = String64("child"), .parentIndex = 1, .nodeIndex = 1}, {.name = String64("parent"), .nodeIndex = 0}}
            };
            const std::array<COM::PrefabSkeletonInfo, 1> skeletons {{{.name = String64("reverse"), .joints = {0, 2}}}};
            ExpectTrue(
                assets
                    .RegisterPrefab(
                        COM::InChain(COM::PrefabRegistrationInfo {.virtualPath = "reverse-joints", .nodes = nodes, .skeletons = skeletons, .joints = joints})
                    )
                    .HasValue()
            );
            joints[1].parentIndex = 0;
            ExpectTrue(
                !assets
                     .RegisterPrefab(
                         COM::InChain(COM::PrefabRegistrationInfo {.virtualPath = "cyclic-joints", .nodes = nodes, .skeletons = skeletons, .joints = joints})
                     )
                     .HasValue()
            );
            const auto cubicText = Test::GLTFCubicFixture;
            auto       cubic     = GLTF::Decode({reinterpret_cast<const uint8_t*>(cubicText.data()), cubicText.size()}, "cubic.gltf");
            if (ExpectTrue(cubic.has_value())) {
                ExpectEq(cubic->animations[0].channels[0].keyValues.size(), size_t(2));
                auto cubicAsset = GLTF::Upload(assets, probe, *cubic, {.buildBLAS = false});
                ExpectTrue(cubicAsset.has_value());
                assets.ClearCache();
                if (cubicAsset)
                    for (const auto texture: cubicAsset->textures)
                        probe.UnloadTexture(texture);
            } else
                assets.ClearCache();
            for (const auto texture: uploaded->textures)
                probe.UnloadTexture(texture);
            return {};
        }
    };
};
} // namespace

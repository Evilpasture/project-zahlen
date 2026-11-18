// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helpers/GLTFFixture.hpp"
#include "helpers/StudioGraphicsProbe.hpp"
#include <Animation/Components.hpp>
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
enum class RecoveryExercise { StateOnly, InitialFrame, RecoveredFrame };
auto ExerciseGPURecovery(RecoveryExercise mode) -> std::expected<void, ZHLN::ErrorCode> {
    using namespace ZHLN;
    using Test::ExpectEq;
    using Test::ExpectTrue;
    auto created = COM::CreateEngineRef(
        COM::InChain(
            COM::EngineAppConfig {.appName = String64("COM glTF recovery"), .logLevel = LogLevel::Quiet},
            COM::EngineWindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::EngineRenderConfig {.validationMode = ValidationMode::Off}
        )
    );
    if (!ExpectTrue(created.has_value()))
        return {};
    auto  engine = std::move(*created);
    auto& assets = *engine->Assets();
    auto& gfx    = *engine->Graphics();
    auto& ecs    = *engine->Entities();
    auto  doc    = Fixture("recover.gltf");
    if (!ExpectTrue(doc.has_value()))
        return {};
    doc->parts.push_back(doc->parts[0]); // two instances of one uploaded primitive
    auto asset = GLTF::Upload(assets, gfx, *doc, {.buildBLAS = false});
    if (!ExpectTrue(asset.has_value()))
        return {};
    auto hit = GLTF::Upload(assets, gfx, *doc, {.buildBLAS = false});
    if (ExpectTrue(hit.has_value()))
        ExpectTrue(!GLTF::RebuildGPUResources(*engine.Get(), *doc, *hit, {.buildBLAS = false}).has_value());
    ExpectTrue(gfx.AllocateJoints(7).HasValue()); // force a nonzero live palette offset
    auto   entities = assets.Instantiate(asset->hash, {.isAnimated = true});
    Entity root = Entity::Null(), part = Entity::Null();
    for (const auto e: entities) {
        if (const auto* inst = ecs.Get<Components::PrefabInstanceComponent>(e); inst != nullptr && inst->animated)
            root = e;
        if (ecs.Get<Components::MorphTargetComponent>(e))
            part = e;
    }
    if (!ExpectTrue(root != Entity::Null() && part != Entity::Null()))
        return {};
    auto* animator                 = ecs.Attach(root, Animation::AnimatorComponent {.prefabAsset = asset->hash});
    if (!ExpectTrue(animator != nullptr))
        return {};
    animator->currentTrackTime     = .375f;
    animator->currentPlaybackSpeed = 1.25f;
    auto* morph                    = ecs.Get<Components::MorphTargetComponent>(part);
    morph->weights                 = {.42f, 0, 0, 0};
    const auto jointOffset         = ecs.Get<Components::SkeletalMeshComponent>(part)->jointOffset;
    ExpectTrue(jointOffset >= 7);
    const auto meshKey     = ecs.Get<Components::MeshComponent>(part)->meshAsset;
    const auto materialKey = ecs.Get<Components::MeshComponent>(part)->materialAsset;
    const auto original    = gfx.FindMesh(meshKey);
    if (!ExpectTrue(original.HasValue()))
        return {};
    ExpectEq(gfx.FindMesh(assets.AssetHash("recover.gltf#part0")).value.posBuffer, gfx.FindMesh(assets.AssetHash("recover.gltf#part1")).value.posBuffer);
    if (mode == RecoveryExercise::InitialFrame) {
        const std::array<Mat4x4, 1> palette {Mat4x4::Identity()};
        ExpectTrue(gfx.UpdateJointMatrices(jointOffset, palette).HasValue());
        ExpectTrue(engine->Tick(1.f / 60.f).HasValue());
        engine->ClearScene();
        assets.ClearCache();
        for (const auto texture: asset->textures)
            gfx.UnloadTexture(texture);
        return {};
    }
    const std::array<float, 12> scratchData {};
    const auto                  scratch = gfx.CreateBuffer(BufferUsage::SkinScratch, std::as_bytes(std::span(scratchData)), sizeof(float));
    ecs.Get<Components::SkeletalMeshComponent>(part)->skinnedScratch     = scratch;
    ecs.Get<Components::SkeletalMeshComponent>(part)->scratchVertexCount = 3;
    const auto oldOffset                                                 = morph->offset;
    const auto oldRevision                                               = asset->gpuRevision;
    const auto oldTextures                                               = asset->textures;
    // Fail after GPU upload, at commit validation. The old publication
    // and ownership manifest must remain usable and retryable.
    Println("    [recovery] validating failed commit and retry");
    auto incompatible                           = *doc;
    incompatible.primitives[0].activeMorphCount = 0;
    incompatible.primitives[0].morphDeltas.clear();
    ExpectTrue(!GLTF::RebuildGPUResources(*engine.Get(), incompatible, *asset, {.buildBLAS = false}).has_value());
    ExpectEq(asset->gpuRevision, oldRevision);
    ExpectEq(asset->textures.size(), oldTextures.size());
    ExpectEq(asset->textures[0], oldTextures[0]);
    ExpectTrue(gfx.UpdateBuffer(scratch, std::as_bytes(std::span(scratchData))).HasValue());
    ExpectEq(gfx.FindMesh(meshKey).value.posBuffer, original.value.posBuffer);
    ExpectEq(morph->offset, oldOffset);
    ExpectTrue(GLTF::RebuildGPUResources(*engine.Get(), *doc, *asset, {.buildBLAS = false}).has_value());
    ExpectTrue(asset->gpuRevision != oldRevision);
    ExpectTrue(asset->textures[0] != oldTextures[0]);
    ExpectTrue(morph->offset != oldOffset);
    ExpectEq(morph->weights.x, .42f);
    ExpectEq(animator->currentTrackTime, .375f);
    ExpectEq(animator->currentPlaybackSpeed, 1.25f);
    ExpectEq(ecs.Get<Components::SkeletalMeshComponent>(part)->jointOffset, jointOffset);
    ExpectTrue(gfx.FindMesh(meshKey).value.posBuffer != original.value.posBuffer);
    ExpectTrue(gfx.FindMaterial(materialKey).HasValue());
    ExpectEq(ecs.Get<Components::SkeletalMeshComponent>(part)->skinnedScratch, BufferID::Invalid);
    const std::array<std::byte, 4> bytes {};
    ExpectTrue(!gfx.UpdateBuffer(original.value.posBuffer, bytes).HasValue());
    ExpectTrue(!gfx.UpdateBuffer(scratch, bytes).HasValue());
    // Replacing aliases with a stale revision must not retire anything.
    const auto                            now = gfx.FindMesh(meshKey).value;
    std::array<COM::PrefabPartGPUInfo, 2> stale {};
    for (size_t i = 0; i < stale.size(); ++i) {
        const auto key    = std::string("recover.gltf#part") + std::to_string(i);
        const auto meshID = assets.AssetHash(std::string_view(key));
        const auto matID  = assets.AssetHash(std::string_view(key + "_mat"));
        stale[i]          = {
            .meshAsset        = meshID,
            .materialAsset    = matID,
            .mesh             = gfx.FindMesh(meshID).value,
            .material         = gfx.FindMaterial(matID).value,
            .morphOffset      = morph->offset,
            .activeMorphCount = 1
        };
    }
    ExpectTrue(!assets
                    .ReplacePrefabGPUResources(
                        COM::InChain(
                            COM::PrefabGPUReplacementInfo {
                                .hash = asset->hash, .deviceGeneration = engine->DeviceGeneration(), .expectedRevision = oldRevision, .parts = stale
                            }
                        )
                    )
                    .HasValue());
    // With a current revision the same descriptor is still rejected:
    // it borrows buffers/material slots that commit would retire.
    ExpectTrue(!assets
                    .ReplacePrefabGPUResources(
                        COM::InChain(
                            COM::PrefabGPUReplacementInfo {
                                .hash = asset->hash, .deviceGeneration = engine->DeviceGeneration(), .expectedRevision = asset->gpuRevision, .parts = stale
                            }
                        )
                    )
                    .HasValue());
    ExpectTrue(!assets
                    .ReplacePrefabGPUResources(
                        COM::InChain(
                            COM::PrefabGPUReplacementInfo {
                                .hash = asset->hash, .deviceGeneration = engine->DeviceGeneration() + 1, .expectedRevision = asset->gpuRevision, .parts = stale
                            }
                        )
                    )
                    .HasValue());
    ExpectEq(gfx.FindMesh(meshKey).value.posBuffer, now.posBuffer);
    Println("    [recovery] recreating graphics device");
    const auto generation = engine->DeviceGeneration();
    if (!ExpectTrue(engine->RecreateGraphicsDevice().HasValue()))
        return {};
    ExpectEq(engine->DeviceGeneration(), generation + 1);
    ExpectTrue(!assets.DescribePrefab(asset->hash).value.gpuResourcesReady);
    ExpectTrue(!GLTF::Upload(assets, gfx, *doc, {.buildBLAS = false}).has_value());
    const auto invalidatedRevision = asset->gpuRevision;
    ExpectTrue(!GLTF::RebuildGPUResources(*engine.Get(), incompatible, *asset, {.buildBLAS = false}).has_value());
    ExpectEq(asset->gpuRevision, invalidatedRevision);
    ExpectTrue(!assets.DescribePrefab(asset->hash).value.gpuResourcesReady);
    ExpectTrue(GLTF::RebuildGPUResources(*engine.Get(), *doc, *asset, {.buildBLAS = false}).has_value());
    ExpectTrue(assets.DescribePrefab(asset->hash).value.gpuResourcesReady);
    ExpectEq(asset->deviceGeneration, engine->DeviceGeneration());
    ExpectEq(animator->currentTrackTime, .375f);
    ExpectEq(morph->weights.x, .42f);
    ExpectEq(ecs.Get<Components::SkeletalMeshComponent>(part)->jointOffset, jointOffset);
    Println("    [recovery] publishing recovered palette");
    const std::array<Mat4x4, 1> joints {Mat4x4::Identity()};
    ExpectTrue(gfx.UpdateJointMatrices(jointOffset, joints).HasValue());
    if (mode == RecoveryExercise::RecoveredFrame)
        ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
    Println("    [recovery] checking future instances and cleanup");
    const auto liveOffset = ecs.Get<Components::MorphTargetComponent>(part)->offset;
    auto       future     = assets.Instantiate(asset->hash, {.isAnimated = true});
    for (const auto e: future)
        if (const auto other = ecs.Get<Components::MorphTargetComponent>(e)) {
            ExpectEq(other->offset, liveOffset);
            ExpectEq(other->weights.x, .75f);
        }
    for (const auto e: future)
        ecs.Despawn(e);
    for (const auto e: entities)
        ecs.Despawn(e);
    assets.ClearCache();
    for (const auto texture: asset->textures)
        gfx.UnloadTexture(texture);
    return {};
}
struct GLTFAssetsSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> rebuild_preserves_live_state_and_survives_renderer_recreation() {
            return ExerciseGPURecovery(RecoveryExercise::StateOnly);
        }
        std::expected<void, ZHLN::ErrorCode> initial_skin_morph_frame() {
            return ExerciseGPURecovery(RecoveryExercise::InitialFrame);
        }
        std::expected<void, ZHLN::ErrorCode> recovered_skin_morph_frame() {
            return ExerciseGPURecovery(RecoveryExercise::RecoveredFrame);
        }

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
            // Source fixtures must not depend on the worker's current directory.
            const auto path       = std::filesystem::path(ZHLN_TEST_SOURCE_DIR) / "resources/assets/ProceduralAnimationBaseRig.glb";
            const auto sourcePath = path.string();
            auto       doc        = GLTF::Read(std::string_view(sourcePath));
            if (!doc)
                Println("    glTF rig fixture failed: '{}' (exists={}, error={})", sourcePath, std::filesystem::exists(path), doc.error());
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

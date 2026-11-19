// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Config.hpp>
#include <array>
#include <vector>
#include <expected>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>
#include <system_error>

namespace {
struct COMPhase2Suite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> independent_phase2_boundary_validation() {
            using namespace ZHLN;
            using namespace ZHLN::COM;
            using Test::ExpectTrue;
            using Test::ExpectEq;
            auto created = CreateEngineRef(InChain(
                AppConfig {.appName = String64("Phase 2 boundary"), .logLevel = LogLevel::Quiet},
                WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
                RenderConfig {.validationMode = ValidationMode::Off}));
            // Unlike the historical GPU-heavy boundary test, init failure is
            // a test failure, not a silently successful skipped assertion.
            if (!ExpectTrue(created.has_value())) return {};
            auto engine = std::move(*created);
            auto debug = engine.As<IDebugDraw>();
            auto graphics = engine.As<IGraphicsDevice>();
            auto assets = engine.As<IAssetLibrary>();
            auto actions = engine.As<IActionContext>();
            if (!ExpectTrue(bool(debug) && bool(graphics) && bool(assets) && bool(actions))) return {};
            ExpectTrue(debug.Get() == engine->Debug());
            engine->SetMaxFramesPerSecond(75);
            ExpectEq(engine->MaxFramesPerSecond(), uint32_t {75});
            engine->SetMaxFramesPerSecond(0);
            ExpectEq(engine->MaxFramesPerSecond(), uint32_t {0});
            const auto frame = engine->CurrentFrame();
            ExpectEq(engine->CurrentFrame(), frame);
            ExpectTrue(!engine->AddDeviceLostCallback(nullptr, nullptr).HasValue());
            const auto listener = engine->AddDeviceLostCallback(+[](void*) {}, nullptr);
            ExpectTrue(listener.HasValue());
            if (listener.HasValue()) engine->RemoveDeviceLostCallback(listener.value);
            engine->RemoveDeviceLostCallback(0);
            engine->RemoveDeviceLostCallback(UINT64_MAX);

            ExpectTrue(!debug->SubmitParticles(nullptr).HasValue());
            ExpectTrue(!debug->SubmitParticles(ParticleEmitterSubmission {}).HasValue());
            ExpectTrue(!debug->SubmitMeshParticles(MeshParticleEmitterSubmission {}).HasValue());
            ExpectTrue(!debug->AddBillboards(TextureID::Invalid, {nullptr, 1}, false).HasValue());
            ExpectTrue(!debug->AddDebugTriangles({nullptr, 1}, {}).HasValue());
            ExpectTrue(debug->AddDebugTriangles({}, {}).HasValue());
            debug->AddLine({}, {1, 2, 3}, {1, 1, 1, 1});
            debug->Clear();

            ExpectTrue(!engine->AddWindow(WindowCreateInfo {.title = {nullptr, 2}}).HasValue());
            ExpectTrue(!engine->SetClipboardText({nullptr, 1}).HasValue());
            ExpectTrue(!engine->ClipboardText(nullptr, 1).HasValue());
            ExpectTrue(!engine->SetFileDropCallback(static_cast<WindowID>(0x7fffffffu), nullptr, nullptr).HasValue());
            ExpectTrue(engine->SetFileDropCallback(WindowID::Primary, nullptr, nullptr).HasValue());
            ExpectTrue(!engine->SubmitUI(WindowID::Primary, UISubmitInfo {.positions = {nullptr, 1}}).HasValue());
            ExpectTrue(engine->SubmitUI(WindowID::Primary, UISubmitInfo {}).HasValue());
            ExpectTrue(!graphics->CreateTextureCube(InChain(TextureCubeCreateInfo {})).HasValue());
            ExpectTrue(!graphics->CreateRenderTexture(InChain(RenderTextureCreateInfo {})).HasValue());
            ExpectTrue(!graphics->SetEnvironmentRadiance(InChain(EnvironmentRadianceInfo {})).HasValue());
            ExpectTrue(!graphics->CaptureFrame(nullptr, {}).HasValue());
            ExpectTrue(!graphics->AllocateJoints(0).HasValue());
            ExpectTrue(!graphics->AllocateJoints(8193).HasValue());
            const auto joints = graphics->AllocateJoints(2);
            ExpectTrue(joints.HasValue());
            ExpectTrue(!graphics->UpdateJointMatrices(8192, {nullptr, 1}).HasValue());
            ExpectTrue(!graphics->AllocateMorphDeltas({nullptr, 1}).HasValue());
            const std::array<float, 3> malformedMorph {};
            ExpectTrue(!graphics->AllocateMorphDeltas(malformedMorph).HasValue());

            ExpectTrue(!actions->SerializeBindings(nullptr, 1).HasValue());
            ExpectTrue(!actions->DeserializeBindings({nullptr, 1}).HasValue());
            const auto size = actions->SerializeBindings(nullptr, 0);
            if (ExpectTrue(size.HasValue() && size.value > 1)) {
                char sentinel = 'Q';
                const auto shortCopy = actions->SerializeBindings(&sentinel, 1);
                ExpectTrue(shortCopy.HasValue() && shortCopy.value == size.value && sentinel == 'Q');
                std::vector<char> text(size.value);
                ExpectTrue(actions->SerializeBindings(text.data(), size.value).HasValue());
                ExpectTrue(actions->DeserializeBindings({text.data(), text.size()}).HasValue());
            }
            actions->BeginBindingCapture(kPrimarySeat, false, KeyCode::Escape);
            actions->CancelBindingCapture();
            ExpectTrue(!actions->TakeCapturedBinding().HasValue());

            const auto temp = std::filesystem::temp_directory_path() / ("zahlen-com-phase2-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(temp);
            struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup {temp};
            { std::ofstream file(temp / "phase2-probe.bin", std::ios::binary); file.write("ABCDE", 5); }
            { std::ofstream file(temp / "phase2-empty.bin", std::ios::binary); }
            const auto mountPath = temp.string();
            ExpectTrue(assets->MountDirectory({mountPath.data(), mountPath.size()}).HasValue());
            ExpectTrue(assets->VirtualFileExists("phase2-probe.bin"));
            const auto fileSize = assets->ReadVirtualFile("phase2-probe.bin", {});
            ExpectTrue(fileSize.HasValue() && fileSize.value == 5);
            uint8_t untouched = 91;
            const auto shortRead = assets->ReadVirtualFile("phase2-probe.bin", {&untouched, 1});
            ExpectTrue(shortRead.HasValue() && shortRead.value == 5 && untouched == 91);
            std::array<uint8_t, 5> fileBytes {};
            ExpectTrue(assets->ReadVirtualFile("phase2-probe.bin", fileBytes).HasValue());
            ExpectTrue(fileBytes[0] == 'A' && fileBytes[4] == 'E');
            const auto emptySize = assets->ReadVirtualFile("phase2-empty.bin", {});
            ExpectTrue(emptySize.HasValue() && emptySize.value == 0);
            ExpectTrue(assets->UnmountDirectory({mountPath.data(), mountPath.size()}).HasValue());
            ExpectTrue(!assets->VirtualFileExists("phase2-probe.bin"));

            ExpectTrue(!assets->MountDirectory({nullptr, 1}).HasValue());
            ExpectTrue(!assets->MountPak({}).HasValue());
            ExpectTrue(!assets->VirtualFileExists("__phase2_missing_file__"));
            ExpectTrue(!assets->ReadVirtualFile("__phase2_missing_file__", {}).HasValue());
            const std::array<FVec3, 3> vertices {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
            const std::array<uint32_t, 3> indices {0, 1, 2};
            const auto collisionID = static_cast<CollisionAssetID>(0xabcdef0123456ULL);
            ExpectTrue(assets->CacheCollisionMesh(InChain(CollisionMeshInfo {collisionID, vertices, indices})).HasValue());
            CollisionMeshInfo collisionOut;
            ExpectTrue(assets->GetCollisionMesh(collisionID, OutChain(collisionOut)).HasValue());
            ExpectEq(collisionOut.vertices.size, size_t {3});
            ExpectEq(collisionOut.indices.size, size_t {3});
            ExpectTrue(!assets->CacheEnvironmentImage(InChain(EnvironmentImageCacheInfo {})).HasValue());
            const std::array<float, 4> rgba {1, 0.5f, 0.25f, 1};
            ExpectTrue(assets->CacheEnvironmentImage(InChain(EnvironmentImageCacheInfo {.key = "phase2/environment", .extent = {1, 1}, .rgba = rgba})).HasValue());
            EnvironmentImageCacheInfo environmentOut;
            ExpectTrue(assets->GetEnvironmentImage("phase2/environment", OutChain(environmentOut)).HasValue());
            ExpectEq(environmentOut.rgba.size, size_t {4});
            if (environmentOut.rgba.size == 4) ExpectEq(environmentOut.rgba.data[1], 0.5f);

            std::array<PrefabNodeInfo, 1> nodes {};
            nodes[0].parentIndex = 0;
            PrefabRegistrationInfo prefab {.virtualPath = "phase2/authored", .nodes = nodes};
            ExpectTrue(!assets->RegisterPrefab(InChain(prefab)).HasValue());
            ExpectTrue(!assets->HasPrefab(assets->PrefabHash(prefab.virtualPath)));
            nodes[0].parentIndex = -1;
            const auto registered = assets->RegisterPrefab(InChain(prefab));
            if (ExpectTrue(registered.HasValue())) {
                ExpectTrue(assets->HasPrefab(registered.value));
                const auto description = assets->DescribePrefab(registered.value);
                ExpectTrue(description.HasValue() && description.value.nodeCount == 1);
                ExpectTrue(!assets->RegisterPrefab(InChain(prefab)).HasValue());
            }
            const auto remainingJoints = graphics->AllocateJoints(8190);
            ExpectTrue(remainingJoints.HasValue());
            ExpectTrue(!graphics->AllocateJoints(1).HasValue());
            // Exhaustion must stay exhausted, never wrap back onto live joints.
            ExpectTrue(!graphics->AllocateJoints(1).HasValue());
            assets->ClearCache();
            ExpectTrue(!assets->GetCollisionMesh(collisionID, OutChain(collisionOut)).HasValue());
            ExpectTrue(!assets->GetEnvironmentImage("phase2/environment", OutChain(environmentOut)).HasValue());
            return {};
        }
    };
};
}

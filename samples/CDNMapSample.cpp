// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Fetch one .glb through ZHLN::CDN::Manager, upload through GLTFAssets, fly with FreeCam.

#include <Animation/Context.hpp>
#include <Animation/AnimationSystem.hpp>
#include <Animation/Animator.hpp>
#include <CDN/CDN.hpp>
#include <FreeCam/FreeCam.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <glTF/GLTFAssets.hpp>
#include <glTF/GLTFDocument.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace {

using ZHLN::FVec3;

inline constexpr std::string_view kBaseURL = "https://pub-59179c82c36c49338ceec8b7926c93cd.r2.dev";
inline constexpr std::string_view kAsset   = "MD_Map_Lights.glb";
inline constexpr uint32_t         kTimeout = 180;

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::Platform::Init();
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 4096, .maxBodyPairs = 8192, .maxContactConstraints = 8192},
        .appName = "Zahlen :: CDN Map",
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig);
    if (!optionsRes) {
        ZHLN::LogError("[Sample] command line: {}", ZHLN::Error(optionsRes.error()).Message());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(engineConfig.logLevel);
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::COM::CreateEngineRef(engineConfig);
    if (!engineRes) {
        ZHLN::LogError("Failed to initialize Engine: {}", ZHLN::Error(engineRes.error()).Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(*engineRes);
    if (!engineConfig.headless) {
        engine->FocusWindow(engine->PrimaryWindow());
    }

    ZHLN::CDN::Config config {
        .baseURL        = kBaseURL,
        .timeoutSeconds = kTimeout,
    };
    auto cdn = ZHLN::CDN::Manager::Create(config);
    if (!cdn) {
        ZHLN::LogError("[CDNMap] CDN::Manager::Create failed: {}", ZHLN::Error(cdn.error()).Message());
        return EXIT_FAILURE;
    }

    ZHLN::Log("[CDNMap] Fetching {}/{}", kBaseURL, kAsset);
    auto bytes = cdn->Load(kAsset, ZHLN::Remote::Validators::IsGLB);
    if (!bytes) {
        ZHLN::LogError("[CDNMap] Load failed: {}", ZHLN::Error(bytes.error()).Message());
        return EXIT_FAILURE;
    }
    ZHLN::Log("[CDNMap] {} KiB", bytes->size() / 1024U);

    auto doc = ZHLN::GLTF::Decode(std::span<const uint8_t>(*bytes), kAsset, {}, {}, engine->Assets());
    if (!doc) {
        ZHLN::LogError("[CDNMap] glTF decode failed for '{}': {}", kAsset, ZHLN::Error(doc.error()).Message());
        return EXIT_FAILURE;
    }
    auto asset = ZHLN::GLTF::Upload(*engine->Assets(), *engine->Graphics(), *doc);
    if (!asset) {
        ZHLN::LogError("[CDNMap] glTF upload failed for '{}': {}", kAsset, ZHLN::Error(asset.error()).Message());
        return EXIT_FAILURE;
    }

    const uint32_t            capacity = 1U + (static_cast<uint32_t>(doc->parts.size()) * 2U) + static_cast<uint32_t>(doc->lights.size());
    std::vector<ZHLN::Entity> instances(capacity);
    const ZHLN::PrefabFactory::SpawnParams   spawnParams {
        .position      = {0.0, 0.0, 0.0},
        .createPhysics = false,
        .isAnimated    = !doc->animations.empty(),
    };
    instances = engine->Assets()->Instantiate(asset->hash, spawnParams);
    const auto written = static_cast<uint32_t>(instances.size());
    instances.resize(std::min<size_t>(written, capacity));
    if (!doc->animations.empty() && !instances.empty()) {
        engine->Entities()->Attach(instances.front(), ZHLN::Animation::AnimatorComponent {.prefabAsset = asset->hash, .currentTrackIdx = 0, .currentLoop = true});
    }
    ZHLN::Log("[CDNMap] Spawned {} instance(s), {} part(s).", written, doc->parts.size());

    FVec3 boundsMin {-1.0f, -1.0f, -1.0f};
    FVec3 boundsMax {1.0f, 1.0f, 1.0f};
    { ZHLN::COM::PrefabBoundsInfo b{}; if (engine->Assets()->GetPrefabBounds(asset->hash, ZHLN::COM::OutChain(b)).HasValue() && b.hasParts) { boundsMin = b.min; boundsMax = b.max; } }
    const FVec3 center    = (boundsMin + boundsMax) * 0.5f;
    const float radius    = std::max((boundsMax - boundsMin).Length() * 0.5f, 1.0f);
    const float moveSpeed = std::clamp(radius * 0.35f, 8.0f, 400.0f);
    ZHLN::FreeCam::Attach(*engine->Entities(), moveSpeed);
    ZHLN::Log("[CDNMap] Bounds centre ({:.1f}, {:.1f}, {:.1f}), radius {:.1f} m.", center.x, center.y, center.z, radius);

    {
        auto& entities = *engine->Entities();
        ZHLN::GraphicsSettings gfx      = engine->Graphics()->Settings();
        gfx.post.exposure               = 0.12f;
        gfx.post.tonemapper             = 1;
        gfx.environment.ambientExposure = 2.0f;
        gfx.shadows.width               = std::clamp(radius * 8.0f, 32.0f, 4000.0f);
        gfx.shadows.resolution          = 2048;
        engine->Graphics()->ApplySettings(gfx);

        const FVec3        sunPos = center + FVec3(radius * 4.0f, radius * 8.0f, radius * 3.0f);
        const ZHLN::Mat4x4 world  = ZHLN::ComposeTransform(sunPos, ZHLN::Quat::Identity(), FVec3::One());
        entities.Spawn(
            ZHLN::Components::NameComponent {.name = ZHLN::String64("MapSun")},
            ZHLN::Components::TransformComponent {.position = sunPos, .rotation = ZHLN::Quat::Identity(), .scale = FVec3::One()},
            ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
            ZHLN::Components::LightComponent {
                .type      = ZHLN::LightType::Sun,
                .color     = FVec3(1.00f, 0.97f, 0.92f),
                .intensity = 12.0f,
                .direction = FVec3(0.35f, 1.00f, 0.25f).Normalized(),
            }
        );

        const ZHLN::Entity camera = entities.SingletonEntity<ZHLN::Components::MainCameraTagComponent>().ValueOr(ZHLN::Entity::Null());
        if (camera != ZHLN::Entity::Null()) {
            const FVec3 eye   = center + FVec3(radius * 0.9f, std::max(radius * 0.35f, 8.0f), radius * 0.9f);
            const FVec3 dir   = (center - eye).Normalized();
            const float yaw   = ZHLN::RadF {std::atan2(dir.z, dir.x)}.ToDegrees().value;
            const float pitch = ZHLN::RadF {std::asin(std::clamp(dir.y, -0.99f, 0.99f))}.ToDegrees().value;
            entities.Patch<ZHLN::Components::CameraComponent>(camera, [&](auto& cc) {
                cc.camera.position = eye;
                cc.camera.yaw      = ZHLN::DegF{yaw};
                cc.camera.pitch    = ZHLN::DegF{pitch};
                cc.camera.nearZ    = std::clamp(radius * 0.002f, 0.05f, 2.0f);
                cc.camera.farZ     = std::max(radius * 12.0f, 200.0f);
            });
        }
    }

    ZHLN::Animation::Context animCtx {};
    uint64_t lastGeneration = engine->DeviceGeneration();
    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    while (engine->IsRunning()) {
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();
        if (engine->Actions()->KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine->RequestClose();
            break;
        }
        if (engine->DeviceGeneration() != lastGeneration) {
            if (ZHLN::GLTF::RebuildGPUResources(*engine, *doc, *asset)) {
                lastGeneration = engine->DeviceGeneration();
            }
        }
        const float dt = clock.Tick();
        ZHLN::Animation::Step(*engine, animCtx, dt);
        ZHLN::FreeCam::Step(*engine, dt);

        const auto result = engine->Tick(dt);
        if (!result.HasValue()) {
            ZHLN::LogError("[CDNMap] frame failed: {}", ZHLN::Error(result.error).Message());
            break;
        }
    }

    for (ZHLN::Entity e : instances) {
        if (engine->Entities()->IsAlive(e)) engine->Entities()->Despawn(e);
    }
    if (asset->ownsResources) {
        for (ZHLN::TextureID tex : asset->textures) {
            if (tex != ZHLN::TextureID::Invalid) engine->Graphics()->UnloadTexture(tex);
        }
    }
    return EXIT_SUCCESS;
}

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/CDNMapSample.cpp
//
// Fetch one .glb through ZHLN::CDN::CDNManager, import it, keep core's free-cam.
//
//   ./build/samples/CDNMapSample
//
// WASD + mouse: the default-scene free-cam. Escape / window close to quit.

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Animation/AnimationSystem.hpp>
#include <CDN/CDN.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <FreeCam/FreeCam.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <glTF/GLTFImporter.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

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

[[nodiscard]] auto NodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex) -> ZHLN::Mat4x4 {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
        return ZHLN::Mat4x4::Identity();
    }
    ZHLN::Mat4x4 transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, FVec3& outMin, FVec3& outMax) {
    outMin = FVec3(1e9f, 1e9f, 1e9f);
    outMax = FVec3(-1e9f, -1e9f, -1e9f);
    for (const ZHLN::ModelPart& part: prefab.parts) {
        const ZHLN::Mat4x4 model = NodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const FVec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const FVec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const FVec3  localCenter = (localMin + localMax) * 0.5f;
        const FVec3  halfExtent  = (localMax - localMin) * 0.5f;
        const FVec3  center      = ZHLN::TransformPoint(model, localCenter);
        const FVec3  extent      = (model.Column(0).XYZ() * halfExtent.x).Abs() + (model.Column(1).XYZ() * halfExtent.y).Abs() +
                                       (model.Column(2).XYZ() * halfExtent.z).Abs();
        outMin = {std::min(outMin.x, center.x - extent.x), std::min(outMin.y, center.y - extent.y), std::min(outMin.z, center.z - extent.z)};
        outMax = {std::max(outMax.x, center.x + extent.x), std::max(outMax.y, center.y + extent.y), std::max(outMax.z, center.z + extent.z)};
    }
    if (prefab.parts.empty()) {
        outMin = FVec3(-1.0f, -1.0f, -1.0f);
        outMax = FVec3(1.0f, 1.0f, 1.0f);
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 4096, .maxBodyPairs = 8192, .maxContactConstraints = 8192},
        .appName = "Zahlen :: CDN Map",
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig);
    if (!optionsRes) {
        ZHLN::LogError("[Sample] command line: {}", optionsRes.error());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(engineConfig.logLevel);

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::Engine::Create(engineConfig);
    if (!engineRes) {
        ZHLN::LogError("Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!engineConfig.headless) {
        engine->GetPlatformHost().Focus();
    }

    ZHLN::FreeCam::Install(*engine);
    ZHLN::Animation::Install(*engine);

#if defined(ZHLN_HAS_FONTS)
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    ZHLN::GLTF::InstallDeviceLostHandler(*engine);

    ZHLN::CDN::CDNConfig config {
        .baseURL        = kBaseURL,
        .timeoutSeconds = kTimeout,
    };
    auto cdn = ZHLN::CDN::CDNManager::Create(config);
    if (!cdn) {
        ZHLN::LogError("[CDNMap] CDNManager::Create failed: {}", cdn.error());
        return EXIT_FAILURE;
    }

    ZHLN::Log("[CDNMap] Fetching {}/{}", kBaseURL, kAsset);
    auto bytes = cdn->Load(kAsset, ZHLN::Remote::Validators::IsGLB);
    if (!bytes) {
        ZHLN::LogError("[CDNMap] Load failed: {}", bytes.error());
        return EXIT_FAILURE;
    }
    ZHLN::Log("[CDNMap] {} KiB", bytes->size() / 1024U);

    auto prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(*engine, std::span<const uint8_t>(*bytes), kAsset);
    if (!prefab) {
        ZHLN::LogError("[CDNMap] glTF import failed for '{}'", kAsset);
        return EXIT_FAILURE;
    }

    const uint32_t            capacity = 1U + (static_cast<uint32_t>(prefab->parts.size()) * 2U);
    std::vector<ZHLN::Entity> instances(capacity);
    const uint32_t            written = ZHLN::PrefabFactory::InstantiatePrefab(
        *engine, *prefab,
        ZHLN::PrefabFactory::SpawnParams {
            .position      = {0.0, 0.0, 0.0},
            .createPhysics = false,
            .isAnimated    = !prefab->animations.empty(),
        },
        instances.data(), capacity
    );
    instances.resize(std::min(written, capacity));
    ZHLN::Log("[CDNMap] Spawned {} instance(s), {} part(s).", written, prefab->parts.size());

    FVec3 boundsMin;
    FVec3 boundsMax;
    ComputeBounds(*prefab, boundsMin, boundsMax);
    const FVec3 center    = (boundsMin + boundsMax) * 0.5f;
    const float     radius    = std::max((boundsMax - boundsMin).Length() * 0.5f, 1.0f);
    const float     moveSpeed = std::clamp(radius * 0.35f, 8.0f, 400.0f);
    ZHLN::FreeCam::Attach(*engine, moveSpeed);
    ZHLN::Log("[CDNMap] Bounds centre ({:.1f}, {:.1f}, {:.1f}), radius {:.1f} m.", center.x, center.y, center.z, radius);

    {
        auto entities = engine->Entities();
        // Default exposure is 1.0; a sun of 60 (the studio number at exposure 0.08)
        // clips the whole framebuffer to white. Grade for an outdoor map.
        engine->Graphics().PatchSettings([radius](auto& gfx) {
            gfx.post.exposure               = 0.12f;
            gfx.post.tonemapper             = 1;
            gfx.environment.ambientExposure = 2.0f;
            gfx.shadows.width               = std::clamp(radius * 8.0f, 32.0f, 4000.0f);
            gfx.shadows.resolution          = 2048;
        });

        const FVec3  sunPos = center + FVec3(radius * 4.0f, radius * 8.0f, radius * 3.0f);
        const ZHLN::Mat4x4 world = ZHLN::ComposeTransform(sunPos, ZHLN::Quat::Identity(), FVec3::One());
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

        const ZHLN::Entity camera = entities.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camera != ZHLN::Entity::Null()) {
            const FVec3 eye   = center + FVec3(radius * 0.9f, std::max(radius * 0.35f, 8.0f), radius * 0.9f);
            const FVec3 dir   = (center - eye).Normalized();
            const float     yaw   = ZHLN::RadF {std::atan2(dir.z, dir.x)}.ToDegrees().value;
            const float     pitch = ZHLN::RadF {std::asin(std::clamp(dir.y, -0.99f, 0.99f))}.ToDegrees().value;
            entities.Patch<ZHLN::Components::CameraComponent>(camera, [&](auto& cc) -> auto {
                cc.camera.position = eye;
                cc.camera.yaw      = ZHLN::DegF{yaw};
                cc.camera.pitch    = ZHLN::DegF{pitch};
                cc.camera.nearZ    = std::clamp(radius * 0.002f, 0.05f, 2.0f);
                cc.camera.farZ     = std::max(radius * 12.0f, 200.0f);
            });
        }
    }

    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    while (engine->IsRunning()) {
        ZHLN::CheckForCrashes(crashState, engine.get());
        engine->ProcessEvents();
        const float dt = clock.Tick();

        {
            auto entities = engine->Entities();
            bool resized  = false;
            if (const auto st = entities.GetSingleton<ZHLN::Components::InputStateComponent>(); st && st->needsResize) {
                engine->Graphics().SetResolution(st->newSize);
                entities.PatchSingleton<ZHLN::Components::InputStateComponent>([](auto& input) { input.needsResize = false; });
                resized = true;
            }
            if (resized) {
                continue;
            }
        }

        const auto result = engine->Tick(dt);
        if (!result) {
            ZHLN::LogError("[CDNMap] frame failed: {}", result.error());
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}

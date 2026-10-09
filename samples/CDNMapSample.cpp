// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/CDNMapSample.cpp
//
// Fetch one .glb through ZHLN::CDN::CDNManager, import it, keep core's free-cam.
//
//   ./build/samples/CDNMapSample
//
// WASD + mouse: the default-scene free-cam. Escape / window close to quit.

#include <CDN/CDN.hpp>
#include <FreeCam/FreeCam.hpp>
#include <RemoteAsset/DiskCache.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <glTF/GLTFImporter.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

namespace {

inline constexpr std::string_view kBaseURL = "https://pub-59179c82c36c49338ceec8b7926c93cd.r2.dev";
inline constexpr std::string_view kAsset   = "MD_Map.glb";
inline constexpr uint32_t         kTimeout = 180;

[[nodiscard]] auto NodeModelTransform(const ZHLN::ModelPrefab& prefab, int32_t nodeIndex) -> JPH::Mat44 {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }
    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, JPH::Vec3& outMin, JPH::Vec3& outMax) {
    outMin = JPH::Vec3(1e9f, 1e9f, 1e9f);
    outMax = JPH::Vec3(-1e9f, -1e9f, -1e9f);
    for (const ZHLN::ModelPart& part: prefab.parts) {
        const JPH::Mat44 model = NodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  localCenter = (localMin + localMax) * 0.5f;
        const JPH::Vec3  halfExtent  = (localMax - localMin) * 0.5f;
        const JPH::Vec3  center      = model.Multiply3x3(localCenter) + model.GetTranslation();
        const JPH::Vec3  extent      = (model.GetColumn3(0) * halfExtent.GetX()).Abs() + (model.GetColumn3(1) * halfExtent.GetY()).Abs() +
                                       (model.GetColumn3(2) * halfExtent.GetZ()).Abs();
        outMin                       = JPH::Vec3::sMin(outMin, center - extent);
        outMax                       = JPH::Vec3::sMax(outMax, center + extent);
    }
    if (prefab.parts.empty()) {
        outMin = JPH::Vec3(-1.0f, -1.0f, -1.0f);
        outMax = JPH::Vec3(1.0f, 1.0f, 1.0f);
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 4096, .maxBodyPairs = 8192, .maxContactConstraints = 8192},
         .render =
             {
                 .appName        = "Zahlen :: CDN Map",
                 .vsync          = options.vsync,
                 .fullscreen     = options.fullscreen,
                 .validationMode = options.validationMode,
                 .headless       = options.headless,
             },
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::LogError("Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!options.headless) {
        engine->GetPlatformHost().Focus();
    }

    ZHLN::FreeCam::Install(*engine);

#if defined(ZHLN_HAS_FONTS)
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    ZHLN::GLTF::InstallDeviceLostHandler(*engine);
    engine->InitializeDefaultScene();

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

    auto prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(engine->GetRenderContext(), engine->GetAssetManager(), std::span<const uint8_t>(*bytes), kAsset);
    if (!prefab) {
        ZHLN::LogError("[CDNMap] glTF import failed for '{}'", kAsset);
        return EXIT_FAILURE;
    }

    const uint32_t            capacity = 1U + (static_cast<uint32_t>(prefab->parts.size()) * 2U);
    std::vector<ZHLN::Entity> instances(capacity);
    const uint32_t            written = ZHLN::PrefabFactory::InstantiatePrefab(
        *engine, *prefab,
        ZHLN::PrefabFactory::SpawnParams {
            .position      = JPH::RVec3(0.0, 0.0, 0.0),
            .createPhysics = false,
            .isAnimated    = !prefab->animations.empty(),
        },
        instances.data(), capacity
    );
    instances.resize(std::min(written, capacity));
    ZHLN::Log("[CDNMap] Spawned {} instance(s), {} part(s).", written, prefab->parts.size());

    JPH::Vec3 boundsMin;
    JPH::Vec3 boundsMax;
    ComputeBounds(*prefab, boundsMin, boundsMax);
    const JPH::Vec3 center    = (boundsMin + boundsMax) * 0.5f;
    const float     radius    = std::max((boundsMax - boundsMin).Length() * 0.5f, 1.0f);
    const float     moveSpeed = std::clamp(radius * 0.35f, 8.0f, 400.0f);
    ZHLN::FreeCam::Attach(*engine, moveSpeed);
    ZHLN::Log("[CDNMap] Bounds centre ({:.1f}, {:.1f}, {:.1f}), radius {:.1f} m.", center.GetX(), center.GetY(), center.GetZ(), radius);

    {
        auto&              reg      = engine->GetRegistry();
        const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
        // Default exposure is 1.0; a sun of 60 (the studio number at exposure 0.08)
        // clips the whole framebuffer to white. Grade for an outdoor map.
        if (settings != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& p) -> auto {
                p.exposure        = 0.12f;
                p.tonemapper      = 1;
                p.ambientExposure = 2.0f;
            });
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settings, [radius](auto& s) -> auto {
                s.shadowWidth      = std::clamp(radius * 8.0f, 32.0f, 4000.0f);
                s.shadowResolution = 2048;
            });
        }

        const JPH::Vec3  sunPos = center + JPH::Vec3(radius * 4.0f, radius * 8.0f, radius * 3.0f);
        const JPH::Mat44 world  = ZHLN::Math::CreateTransform(sunPos, JPH::Quat::sIdentity());
        reg.Create(
            ZHLN::Components::NameComponent {.name = ZHLN::String64("MapSun")},
            ZHLN::Components::TransformComponent {.position = sunPos, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
            ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
            ZHLN::Components::LightComponent {
                .type      = ZHLN::LightType::Sun,
                .color     = JPH::Vec3(1.00f, 0.97f, 0.92f),
                .intensity = 12.0f,
                .direction = JPH::Vec3(0.35f, 1.00f, 0.25f).Normalized(),
            }
        );

        const ZHLN::Entity camera = reg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        if (camera != ZHLN::Entity::Null()) {
            const JPH::Vec3 eye   = center + JPH::Vec3(radius * 0.9f, std::max(radius * 0.35f, 8.0f), radius * 0.9f);
            const JPH::Vec3 dir   = (center - eye).Normalized();
            const float     yaw   = JPH::RadiansToDegrees(std::atan2(dir.GetZ(), dir.GetX()));
            const float     pitch = JPH::RadiansToDegrees(std::asin(std::clamp(dir.GetY(), -0.99f, 0.99f)));
            reg.Patch<ZHLN::Components::CameraComponent>(camera, [&](auto& cc) -> auto {
                cc.camera.position = eye;
                cc.camera.yaw      = yaw;
                cc.camera.pitch    = pitch;
                cc.camera.nearZ    = std::clamp(radius * 0.002f, 0.05f, 2.0f);
                cc.camera.farZ     = std::max(radius * 12.0f, 200.0f);
            });
        }
    }

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        engine->ProcessEvents();
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);

        {
            auto& r = engine->GetRegistry();
            if (auto st = r.GetSingleton<ZHLN::Components::InputStateComponent>(); st && st->needsResize) {
                engine->GetRenderContext().SetResolution(st->newSize);
                st->needsResize = false;
                continue;
            }
        }

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }
    }
    return EXIT_SUCCESS;
}

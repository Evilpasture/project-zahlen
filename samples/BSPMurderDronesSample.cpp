// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/BSPMurderDronesSample.cpp
//
// Standalone sample demonstrating Source Engine world ingestion in Zahlen:
// loads import/engine_assets/maps/gm_murder_drones.bsp, resolves VMT/VTF materials,
// external lightmap atlas, static prop StudioModels (.mdl/.vvd/.vtx/.phy),
// interactive entities (doors, buttons, triggers), and spawns the player at
// info_player_start with free-cam controls.
//
// Usage:
//   ./build/p2996/samples/BSPMurderDronesSample
//   ./build/p2996/samples/BSPMurderDronesSample import/engine_assets/maps/gm_murder_drones.bsp

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPImporter.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Interaction/BSPInteraction.hpp>
#include <Interaction/InteractionSystem.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

inline constexpr std::string_view kDefaultMapPath       = "import/engine_assets/maps/gm_murder_drones.bsp";
inline constexpr std::string_view kDefaultAssetRoot     = "import/engine_assets";
inline constexpr std::string_view kDefaultLightmapAtlas = "import/engine_assets/lightmaps/gm_murder_drones_lightmap0.png";

[[nodiscard]] auto ReadFileBytes(const std::filesystem::path& path) -> std::optional<std::vector<std::byte>> {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    file.seekg(0);
    std::vector<std::byte> out(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

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
        const JPH::Mat44 model       = NodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        const JPH::Vec3  localCenter = (localMin + localMax) * 0.5f;
        const JPH::Vec3  halfExtent  = (localMax - localMin) * 0.5f;
        const JPH::Vec3  center      = model.Multiply3x3(localCenter) + model.GetTranslation();
        const JPH::Vec3  extent      = (model.GetColumn3(0) * halfExtent.GetX()).Abs() +
                                       (model.GetColumn3(1) * halfExtent.GetY()).Abs() +
                                       (model.GetColumn3(2) * halfExtent.GetZ()).Abs();
        outMin                       = JPH::Vec3::sMin(outMin, center - extent);
        outMax                       = JPH::Vec3::sMax(outMax, center + extent);
    }
    if (prefab.parts.empty()) {
        outMin = JPH::Vec3(-10.0f, -10.0f, -10.0f);
        outMax = JPH::Vec3(10.0f, 10.0f, 10.0f);
    }
}

void PrintControlsBanner(std::string_view mapPath) {
    ZHLN::Log("==================================================================");
    ZHLN::Log("       Zahlen Engine :: Source World Ingestion Sample             ");
    ZHLN::Log("==================================================================");
    ZHLN::Log("Map: {}", mapPath);
    ZHLN::Log("Controls:");
    ZHLN::Log("  WASD          - Move camera in viewport");
    ZHLN::Log("  Mouse         - Look around");
    ZHLN::Log("  Space / Ctrl  - Move camera up / down");
    ZHLN::Log("  Left Shift    - Boost camera speed");
    ZHLN::Log("  E             - Interact with doors, buttons, and triggers");
    ZHLN::Log("  Escape        - Quit sample");
    ZHLN::Log("==================================================================");
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& cmdOptions = optionsRes.value();
    if (cmdOptions.helpRequested || cmdOptions.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(cmdOptions.logLevel);

    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    // 1. Resolve map path and companion directories
    std::filesystem::path mapPath = kDefaultMapPath;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (!arg.starts_with("-") && arg.ends_with(".bsp")) {
            mapPath = arg;
            break;
        }
    }

    // Check candidate locations if default not found directly in CWD
    if (!std::filesystem::exists(mapPath)) {
        const std::filesystem::path altCandidate = std::filesystem::path("..") / mapPath;
        if (std::filesystem::exists(altCandidate)) {
            mapPath = altCandidate;
        }
    }

    if (!std::filesystem::exists(mapPath)) {
        ZHLN::LogError("BSP file not found: '{}'", mapPath.string());
        ZHLN::LogError("Please verify that 'import/engine_assets/maps/gm_murder_drones.bsp' exists.");
        return EXIT_FAILURE;
    }

    // Infer asset root from map location (typically <root>/maps/<map>.bsp)
    std::filesystem::path assetRoot = kDefaultAssetRoot;
    if (mapPath.has_parent_path() && mapPath.parent_path().filename() == "maps") {
        assetRoot = mapPath.parent_path().parent_path();
    } else if (!std::filesystem::exists(assetRoot) && std::filesystem::exists(std::filesystem::path("..") / assetRoot)) {
        assetRoot = std::filesystem::path("..") / assetRoot;
    }

    // Infer lightmap atlas path
    std::filesystem::path lightmapPath = kDefaultLightmapAtlas;
    const std::string     mapStem      = mapPath.stem().string();
    const std::filesystem::path candidateLightmap = assetRoot / "lightmaps" / (mapStem + "_lightmap0.png");
    if (std::filesystem::exists(candidateLightmap)) {
        lightmapPath = candidateLightmap;
    } else if (!std::filesystem::exists(lightmapPath) && std::filesystem::exists(std::filesystem::path("..") / lightmapPath)) {
        lightmapPath = std::filesystem::path("..") / lightmapPath;
    }

    ZHLN::Log("[BSPSample] Target map: {}", mapPath.string());
    ZHLN::Log("[BSPSample] Asset root: {}", assetRoot.string());
    if (std::filesystem::exists(lightmapPath)) {
        ZHLN::Log("[BSPSample] Lightmap atlas: {}", lightmapPath.string());
    } else {
        ZHLN::Log("[BSPSample] Lightmap atlas not found on disk, fallback unlit/baked lighting will be used.");
        lightmapPath.clear();
    }

    // 2. Read BSP file bytes
    auto bspBytesOpt = ReadFileBytes(mapPath);
    if (!bspBytesOpt) {
        ZHLN::LogError("[BSPSample] Failed to read BSP file: {}", mapPath.string());
        return EXIT_FAILURE;
    }
    const auto& bspBytes = *bspBytesOpt;
    ZHLN::Log("[BSPSample] Loaded BSP file ({} KiB)", bspBytes.size() / 1024U);

    // 3. Parse BSP headers & lumps
    auto bspMapExp = ZHLN::BSP::ParseBsp(bspBytes);
    if (!bspMapExp) {
        ZHLN::LogError("[BSPSample] Failed to parse BSP: {}", bspMapExp.error());
        return EXIT_FAILURE;
    }
    const auto& bspMap = *bspMapExp;
    ZHLN::Log("[BSPSample] Map version: {}", bspMap.version);
    ZHLN::Log("[BSPSample] Entities: {}, Static Props: {}, Models: {}, Displacements: {}",
        bspMap.entities.size(), bspMap.staticProps.size(), bspMap.models.size(), bspMap.dispInfos.size());

    // 4. Initialize Engine
    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 16384, .maxBodyPairs = 32768, .maxContactConstraints = 16384},
         .render =
             {
                 .appName        = "Zahlen :: gm_murder_drones",
                 .vsync          = cmdOptions.vsync,
                 .fullscreen     = cmdOptions.fullscreen,
                 .validationMode = cmdOptions.validationMode,
                 .headless       = cmdOptions.headless,
             },
         .enableFallbackScene = false}
    );
    if (!engineRes) {
        ZHLN::LogError("[BSPSample] Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!cmdOptions.headless) {
        engine->GetPlatformHost().Focus();
    }

    // Install systems
    ZHLN::FreeCam::Install(*engine);
    ZHLN::Interaction::Install(*engine);

#if defined(ZHLN_HAS_FONTS)
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("[BSPSample] Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    engine->InitializeDefaultScene();

    // 5. Import BSP into ModelPrefab
    ZHLN::BSP::ImportOptions importOpts {
        .convertCoordinates   = true,
        .unitScale            = 0.0254f, // Convert inches to meters
        .includeDisplacements = true,
        .gatherLights         = true,
        .buildColliders       = true,
        .assetRoot            = assetRoot.string(),
        .lightmapAtlasPath    = lightmapPath.string(),
    };

    const std::string virtualPath = mapPath.string();
    ZHLN::Log("[BSPSample] Importing BSP geometry and resolving materials...");
    auto prefabOpt = ZHLN::BSP::LoadBSPPrefabFromMemory(
        engine->GetRenderContext(),
        engine->GetAssetManager(),
        bspBytes,
        virtualPath,
        importOpts
    );
    if (!prefabOpt) {
        ZHLN::LogError("[BSPSample] Failed to import BSP prefab for '{}'", virtualPath);
        return EXIT_FAILURE;
    }
    const auto& prefab = *prefabOpt;
    ZHLN::Log("[BSPSample] Imported prefab: {} part(s), {} node(s), {} light(s)",
        prefab.parts.size(), prefab.nodes.size(), prefab.lights.size());

    // 6. Describe and Instantiate Scene
    ZHLN::Log("[BSPSample] Translating BSP entities and static props to Scene...");
    const ZHLN::Scene::Scene sceneDesc = ZHLN::BSP::DescribeScene(bspMap, virtualPath, importOpts);
    ZHLN::Log("[BSPSample] Scene description ready: {} entity(ies), {} light(s)",
        sceneDesc.entities.size(), sceneDesc.lights.size());

    auto instanceExp = ZHLN::Scene::Instantiate(*engine, sceneDesc);
    if (!instanceExp) {
        ZHLN::LogError("[BSPSample] Failed to instantiate scene: {}", instanceExp.error());
        return EXIT_FAILURE;
    }
    const auto& instance = *instanceExp;
    ZHLN::Log("[BSPSample] Instantiated {} scene entities and {} lights",
        instance.entities.size(), instance.lights.size());

    // 7. Initialize Interactive Entities (doors, buttons, triggers)
    ZHLN::Interaction::InitializeBSPEntities(engine->GetRegistry(), bspMap, instance, importOpts);
    ZHLN::Log("[BSPSample] Interactive entity components attached and wired.");

    // 8. Configure Camera & Viewport
    JPH::Vec3 boundsMin;
    JPH::Vec3 boundsMax;
    ComputeBounds(prefab, boundsMin, boundsMax);
    const JPH::Vec3 mapCenter = (boundsMin + boundsMax) * 0.5f;
    const float     mapRadius = std::max((boundsMax - boundsMin).Length() * 0.5f, 10.0f);
    const float     moveSpeed = std::clamp(mapRadius * 0.08f, 12.0f, 150.0f);

    ZHLN::FreeCam::Attach(*engine, moveSpeed);
    ZHLN::Log("[BSPSample] Map bounds: min=({:.1f}, {:.1f}, {:.1f}), max=({:.1f}, {:.1f}, {:.1f}), radius={:.1f} m",
        boundsMin.GetX(), boundsMin.GetY(), boundsMin.GetZ(),
        boundsMax.GetX(), boundsMax.GetY(), boundsMax.GetZ(),
        mapRadius);
    ZHLN::Log("[BSPSample] FreeCam initialized at {:.1f} m/s", moveSpeed);

    // 9. Lighting & Post-Processing Setup
    {
        auto&              reg      = engine->GetRegistry();
        const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings != ZHLN::Entity::Null()) {
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& p) {
                p.exposure        = 0.18f;
                p.tonemapper      = 1;
                p.ambientExposure = 1.2f;
            });
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settings, [mapRadius](auto& s) {
                s.shadowWidth      = std::clamp(mapRadius * 2.5f, 64.0f, 2048.0f);
                s.shadowResolution = 2048;
            });
        }

        // Add ambient sun fill if the map does not have light_environment
        bool hasSun = false;
        for (const auto& ent: bspMap.entities) {
            if (ent.Find("classname") == "light_environment") {
                hasSun = true;
                break;
            }
        }

        if (!hasSun) {
            const JPH::Vec3  sunPos = mapCenter + JPH::Vec3(mapRadius * 1.5f, mapRadius * 2.5f, mapRadius * 1.0f);
            const JPH::Mat44 world  = ZHLN::Math::CreateTransform(sunPos, JPH::Quat::sIdentity());
            reg.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64("WorldSun")},
                ZHLN::Components::TransformComponent {.position = sunPos, .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)},
                ZHLN::Components::WorldTransformComponent {.world = world, .previous = world},
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(0.95f, 0.96f, 1.00f),
                    .intensity = 8.0f,
                    .direction = JPH::Vec3(-0.4f, 0.85f, -0.35f).Normalized(),
                }
            );
        }
    }

    PrintControlsBanner(mapPath.string());

    // 10. Frame Loop
    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        engine->ProcessEvents();
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);

        {
            auto& reg = engine->GetRegistry();
            if (auto st = reg.GetSingleton<ZHLN::Components::InputStateComponent>(); st && st->needsResize) {
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

    ZHLN::Log("[BSPSample] Exiting cleanly.");
    return EXIT_SUCCESS;
}

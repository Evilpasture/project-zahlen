// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// Optional extras/toolkit modules
import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline const JPH::Vec3 kSunPosition {25.0f, 60.0f, 25.0f};
inline const JPH::Vec3 kSunColor {1.00f, 0.96f, 0.90f};
inline const JPH::Vec4 kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const JPH::Vec4 kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const JPH::Vec4 kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

[[nodiscard]] bool EnvironmentFlag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" && text != "off" && text != "OFF";
}

[[nodiscard]] float EnvironmentFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*       end    = nullptr;
    const float parsed = std::strtof(value, &end);
    return end != value ? parsed : fallback;
}

auto BuildProceduralArena(ZHLN::Engine& engine) -> void {
    auto& reg = engine.GetRegistry();

    // 1. Configure Post-Processing Atmosphere via ECS Patch
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, e, [](auto& pp) -> auto {
            pp.ambientExposure = kAmbientExposure;
            pp.skyZenith       = kSkyZenith;
            pp.skyHorizon      = kSkyHorizon;
            pp.skyGround       = kSkyGround;
        });
    }

    // 2. Terrain (220m procedural rolling landscape)
    ZHLN::CreativeWorksFactory::CreateTerrain(
        engine, 128, 220.0f, 12.0f, ZHLN::CreativeWorksFactory::TerrainType::Default,
        ZHLN::CreativeWorksFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.80f}
    );

    // 3. Center Platform
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(10.0f, 0.50f, 10.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position = {0.0, 0.50, 0.0}, .createPhysics = true, .isStaticPhysics = true, .roughness = 0.50f, .color = {0.32f, 0.34f, 0.38f, 1.0f}
        }
    );

    // 4. 30-Degree Grounding Test Ramp
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(4.5f, 0.18f, 2.2f),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position        = {-14.0, 2.75, 5.0},
            .rotation        = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), JPH::DegreesToRadians(30.0f)),
            .createPhysics   = true,
            .isStaticPhysics = true,
            .roughness       = 0.55f,
            .color           = {0.25f, 0.50f, 0.72f, 1.0f}
        }
    );

    // 5. Stepping Stones
    for (int i = 0; i < 6; ++i) {
        float stepHeight = 0.20f + (static_cast<float>(i) * 0.10f);
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.0f, stepHeight * 0.5f, 1.0f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {12.0 + (static_cast<double>(i) * 2.5), static_cast<double>(stepHeight * 0.5f), -4.0},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .roughness       = 0.45f,
                .color           = {0.65f, 0.40f, 0.20f, 1.0f}
            }
        );
    }

    // 6. Obstacle Pillars
    struct PillarDesc {
        float     x, z, width, height;
        JPH::Vec4 color;
    };
    const std::array<PillarDesc, 4> pillars = {
        {{.x = -14.0f, .z = -8.0f, .width = 1.5f, .height = 5.0f, .color = {0.35f, 0.45f, 0.60f, 1.0f}},
         {.x = -18.0f, .z = 4.0f, .width = 2.0f, .height = 7.5f, .color = {0.30f, 0.40f, 0.55f, 1.0f}},
         {.x = 18.0f, .z = 12.0f, .width = 2.2f, .height = 9.0f, .color = {0.25f, 0.35f, 0.50f, 1.0f}},
         {.x = 14.0f, .z = -16.0f, .width = 2.0f, .height = 8.0f, .color = {0.28f, 0.38f, 0.52f, 1.0f}}}
    };
    for (const auto& p: pillars) {
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(p.width * 0.5f, p.height * 0.5f, p.width * 0.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {static_cast<double>(p.x), static_cast<double>(p.height * 0.5f), static_cast<double>(p.z)},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .color           = p.color
            }
        );
    }

    // 7. Directional Sunlight via Variadic Create
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = kSunColor, .intensity = kSunIntensity, .direction = JPH::Vec3(0.45f, 1.00f, 0.30f).Normalized()
        }
    );
}

/**
 * @brief Attaches a loaded GLB model or generates a procedural fallback rig.
 */
auto AttachCharacterRig(ZHLN::Engine& engine, ZHLN::Entity player, std::string_view glbPath, ZHLN::ModelPrefab* prefab) -> void {
    auto& reg = engine.GetRegistry();

    ZHLN::ProceduralLocomotionComponent locomotion {
        .strideLength = 1.40f,
        .stepHeight   = 0.22f,
        .legReach     = 0.83f,
    };
    ZHLN::HairStrandsComponent      hair {};
    ZHLN::ProceduralLookAtComponent lookAt {
        .targetWorldPos = JPH::Vec3(0.0f, 2.0f, 4.0f),
        .weight         = 0.85f,
        .maxAngleDeg    = 70.0f,
    };
    const char*                              interpolationValue = std::getenv("ZHLN_POSE_INTERPOLATION");
    const bool                               useBicubic         = interpolationValue != nullptr && std::string_view(interpolationValue) == "bicubic";
    ZHLN::ProceduralAnimationConfigComponent animationConfig {
        .poseInterpolation       = useBicubic ? ZHLN::PoseInterpolationMode::Bicubic : ZHLN::PoseInterpolationMode::SpringDamper,
        .springStiffness         = EnvironmentFloat("ZHLN_SPRING_STIFFNESS", 2500.0f),
        .springDampingFactor     = EnvironmentFloat("ZHLN_SPRING_DAMPING_FACTOR", 0.90f),
        .bicubicTension          = EnvironmentFloat("ZHLN_BICUBIC_TENSION", 0.0f),
        .legIKWeight             = EnvironmentFloat("ZHLN_LEG_IK_WEIGHT", 0.65f),
        .pelvisDropWeight        = EnvironmentFloat("ZHLN_PELVIS_DROP_WEIGHT", 1.0f),
        .maxFootHeightCorrection = EnvironmentFloat("ZHLN_MAX_FOOT_HEIGHT_CORRECTION", 0.18f),
        .enableLegIK             = !EnvironmentFlag("ZHLN_DISABLE_IK"),
        .worldLockFeet           = EnvironmentFlag("ZHLN_WORLD_LOCK_FEET"),
        .enableAccelerationTilt  = !EnvironmentFlag("ZHLN_DISABLE_ACCELERATION_TILT"),
        .authoredPoseOnly        = EnvironmentFlag("ZHLN_AUTHORED_POSE_ONLY") || EnvironmentFlag("ZHLN_KEYFRAME_ONLY"),
    };
    ZHLN::Log(
        "[Sample] Pose interpolation: {} (stiffness={}, damping factor={}, bicubic tension={}).", useBicubic ? "bicubic" : "spring-damper",
        animationConfig.springStiffness, animationConfig.springDampingFactor, animationConfig.bicubicTension
    );
    ZHLN::Log(
        "[Sample] Leg IK weight={}; authored X/Z=true; world lock={}; max height correction={}; pelvis-drop weight={}.",
        animationConfig.enableLegIK ? animationConfig.legIKWeight : 0.0f, animationConfig.worldLockFeet, animationConfig.maxFootHeightCorrection,
        animationConfig.pelvisDropWeight
    );
    if (animationConfig.authoredPoseOnly) {
        ZHLN::Log("[Sample] Authored-pose-only isolation enabled; all procedural layers are bypassed.");
    } else if (!animationConfig.enableLegIK) {
        ZHLN::Log("[Sample] Leg IK disabled; gait/keyframe layers remain active.");
    }

    if (prefab != nullptr) {
        ZHLN::Log("[Sample] GLB model '{}' loaded successfully. Instantiating visual parts...", glbPath);

        int32_t idleTrack = ZHLN::FindAnimationTrack(*prefab, "idle");
        if (idleTrack < 0 && !prefab->animations.empty()) {
            idleTrack = 0;
        }
        const int32_t walkTrack = ZHLN::FindAnimationTrack(*prefab, "walk");
        const int32_t runTrack  = ZHLN::FindAnimationTrack(*prefab, "run");
        if (idleTrack >= 0) {
            const auto& clip = prefab->animations[static_cast<size_t>(idleTrack)];
            ZHLN::Log("[Sample] Selected idle track {}: '{}' (duration={}, channels={}).", idleTrack, clip.name, clip.duration, clip.channels.size());
            ZHLN::Log("[Sample] Locomotion tracks: idle={}, walk={}, run={}.", idleTrack, walkTrack, runTrack);
        } else {
            ZHLN::Log("[Sample] WARNING: '{}' contains no authored animation track; bind pose will be shown.", glbPath);
        }

        // Instantiate the visual hierarchy without physics colliders. A prefab
        // can emit one root, one entity per part, and at most one emissive VPL
        // per part. Size the output dynamically: InstantiatePrefab returns the
        // total number spawned even when the caller's output span is smaller.
        const size_t              outputCapacity = 1 + prefab->parts.size() * 2;
        std::vector<ZHLN::Entity> parts(outputCapacity);
        const uint32_t            count = ZHLN::CreativeWorksFactory::InstantiatePrefab(
            engine, *prefab, ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0f, 1.20f, 0.0f), .createPhysics = false, .isAnimated = true},
            parts.data(), static_cast<uint32_t>(parts.size())
        );
        const uint32_t writtenCount = std::min(count, static_cast<uint32_t>(parts.size()));

        // Re-parent every returned visual mesh part directly under the player
        // CharacterVirtual entity. Emissive VPLs have no hierarchy and are
        // safely ignored by Patch.
        for (uint32_t i = 1; i < writtenCount; ++i) {
            ZHLN::ECS::Patch<ZHLN::Components::HierarchyComponent>(reg, parts[i], [&](auto& hier) -> auto { hier.parent = player; });
        }

        // Clean up the redundant prefab container root (parts[0]).
        if (writtenCount > 0) {
            reg.Destroy(parts[0]);
        }

        // Variadic component registration: AnimatorComponent triggers RigBoneMap discovery
        reg.Add(
            player,
            ZHLN::Components::AnimatorComponent {
                .currentTrackIdx = idleTrack,
                .currentLoop     = true,
                .prefab          = prefab,
            },
            ZHLN::ProceduralLocomotionTracksComponent {
                .idleTrack = idleTrack,
                .walkTrack = walkTrack,
                .runTrack  = runTrack,
            },
            ZHLN::Components::KinematicPoseOverrideComponent {}, ZHLN::RigBoneMap {}, // Initialized lazily on frame 0 by the optional subsystem
            std::move(locomotion), std::move(hair), std::move(lookAt), animationConfig
        );
    } else {
        ZHLN::Log("[Sample] Notice: '{}' not found. Falling back to in-memory procedural rig.", glbPath);

        ZHLN::RigBoneMap proceduralRig {};
        ZHLN::BuildStandardProceduralRig(proceduralRig);

        reg.Add(
            player, ZHLN::Components::KinematicPoseOverrideComponent {}, std::move(proceduralRig), std::move(locomotion), std::move(hair), std::move(lookAt),
            animationConfig
        );
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();

    if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();
    ZHLN::DefaultPreset::SetDisabled(true);

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
         .render  = {.appName = "Zahlen :: Procedural Locomotion Sample", .vsync = options.vsync, .fullscreen = options.fullscreen}}
    );

    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();
    ZHLN::ProceduralAnimation::Register(*engine);
    BuildProceduralArena(*engine);

    // 1. Load the visual first so the CharacterVirtual hull can be fitted to
    // every transformed mesh-part bound instead of assuming a fixed human size.
    const char*            rigOverride = std::getenv("ZHLN_PROCEDURAL_RIG");
    const std::string_view rigPath     = rigOverride != nullptr && rigOverride[0] != '\0' ? std::string_view(rigOverride) :
                                                                                            std::string_view("ProceduralAnimationBaseRig.glb");
    ZHLN::Log("[Sample] Using procedural rig '{}'. Set ZHLN_PROCEDURAL_RIG to override.", rigPath);
    ZHLN::ModelPrefab* const prefab = ZHLN::CreativeWorksFactory::LoadModelPrefab(*engine, rigPath);

    const ZHLN::Locomotion::CharacterBoundsEstimate bounds          = prefab != nullptr ? ZHLN::Locomotion::EstimateCharacterBounds(*prefab) :
                                                                                          ZHLN::Locomotion::CharacterBoundsEstimate {};
    const ZHLN::Physics::DualShapeConfig            dualShapeConfig = ZHLN::Locomotion::FitDualShapeToBounds(bounds);
    if (bounds.valid) {
        const JPH::Vec3 size = bounds.Size();
        ZHLN::Log(
            "[Sample] Estimated GLB bounds min=({}, {}, {}), max=({}, {}, {}), size=({}, {}, {}).", bounds.min.GetX(), bounds.min.GetY(), bounds.min.GetZ(),
            bounds.max.GetX(), bounds.max.GetY(), bounds.max.GetZ(), size.GetX(), size.GetY(), size.GetZ()
        );
    }
    ZHLN::Log(
        "[Sample] Character hull: lifter radius={}, bumper radius XZ={}, bumper radius Y={}, top={}.", dualShapeConfig.lifterRadius,
        dualShapeConfig.bumperRadiusXZ, dualShapeConfig.bumperRadiusY, dualShapeConfig.GetBumperOffsetY() + dualShapeConfig.bumperRadiusY
    );

    constexpr float    kWalkSpeed = 2.40f;
    constexpr float    kJumpForce = 7.00f;
    const ZHLN::Entity player     = ZHLN::Locomotion::SpawnCharacter(*engine, JPH::Vec3(0.0f, 1.20f, 0.0f), dualShapeConfig, kWalkSpeed, kJumpForce);

    // 2. Attach the already-loaded visual to the fitted CharacterVirtual.
    AttachCharacterRig(*engine, player, rigPath, prefab);

    ZHLN::Clock clock;
    float       sampleTime = 0.0f;
    ZHLN::Log(
        "[ProceduralAnimationSample] Ready. WASD move, LSHIFT sprint, SPACE jump, Right-Click orbit. "
        "Cyan/orange/green = torso/arms/IK legs; magenta = 108-bone XPBD hair; yellow = terrain normals."
    );

    while (engine->IsRunning()) {
        const auto dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        auto& registry = engine->GetRegistry();

        // 1. Mouse Look Delta via Multi-Component Patch
        for (ZHLN::Entity e: registry.GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            ZHLN::ECS::Patch<ZHLN::Components::InputStateComponent>(registry, e, [&](auto& st) -> auto {
                if (st.needsResize) {
                    engine->GetRenderContext().SetResolution(st.newSize);
                    st.needsResize = false;
                }
                if (st.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    engine->GetCamera().yaw += st.GetMouseDeltaX() * 0.15f;
                    engine->GetCamera().pitch = std::clamp(engine->GetCamera().pitch - (st.GetMouseDeltaY() * 0.15f), -85.0f, 85.0f);
                }
            });
        }

        // 2. Update Procedural Look-At Orbit via Multi-Component Patch
        sampleTime += dt;
        ZHLN::ECS::Patch<ZHLN::Components::TransformComponent, ZHLN::ProceduralLookAtComponent>(registry, player, [&](const auto& trans, auto& lookAt) -> auto {
            lookAt.targetWorldPos = trans.position + trans.rotation * JPH::Vec3(std::sin(sampleTime * 0.65f) * 2.2f, 1.65f, 4.0f);
        });

        // 3. Synchronized Engine Tick
        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        // 4. Render Visual Collision Rig & Evaluated Procedural Skeleton
        ZHLN::Locomotion::RenderDebugRig(*engine, player, dualShapeConfig);

        ZHLN::ECS::Patch<ZHLN::Components::TransformComponent, ZHLN::RigBoneMap>(registry, player, [&](const auto& trans, const auto& rig) -> auto {
            const auto* gait = registry.Get<ZHLN::ProceduralLocomotionComponent>(player);
            ZHLN::ProceduralAnimation::DrawDebugRig(engine->GetRenderContext(), trans.position, trans.rotation, rig, gait);
        });
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}

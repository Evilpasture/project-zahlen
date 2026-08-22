// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TestPerformance.cpp — Full-engine performance stress case.
//
// Unlike the other GPU tests (which verify one feature over a few dozen frames),
// this case builds a large "kitchen sink" scene and drives it for several hundred
// frames, churning every major engine subsystem at the same time:
//
//   * ECS:      sustained entity/component churn (direct + deferred ECB paths),
//               raw-array systems, generation recycling, patching
//   * Fibers:   TaskSystem ParallelFor / Dispatch inside the ECS SystemGraph
//               (multi-threaded system execution), ALife parallel phases (extras)
//   * Physics:  hundreds of dynamic rigid bodies, per-frame spawn/kill churn,
//               radial-impulse explosions, raycast/shape-cast/overlap queries,
//               a dual-shape character steered by a target camera
//   * Render:   cascaded sun shadows + punctual shadow-casting point lights,
//               SSGI, SSR, TAA, volumetric fog, decals, LOD, terrain, sky,
//               hundreds of thousands of GPU-simulated particles (overdraw),
//               skeletal animation (when a real GLB asset is present),
//               hardware ray tracing with per-frame TLAS rebuild (when supported),
//               mesh shaders (when supported), native ECS UI
//   * Audio:    loop synthesizers + procedural one-shots (fire-and-forget)
//   * Script:   periodic Lua IPC through ScriptRunner::ExecuteString
//
// The measured window is reported as a full FPS breakdown (avg / min / p50 / p95 /
// max) plus the per-system CPU profiler roll-up, so you can read the baseline for
// your own hardware straight out of the log.
//
// Environment variables (all optional):
//   ZHLN_PERF_VALIDATION  0/1     Vulkan validation layer (default 0 = off for clean
//                                 numbers; set 1 to also assert VUID cleanliness)
//   ZHLN_PERF_WIDTH       px      (default 1280)
//   ZHLN_PERF_HEIGHT      px      (default 720)
//   ZHLN_PERF_WARMUP      frames  (default 120, excluded from statistics)
//   ZHLN_PERF_FRAMES      frames  (default 600, measured)
//   ZHLN_PERF_SCALE       float   (default 1.0; 0.5 to lighten the load on weak
//                                 GPUs, 2.0 to push harder)
//   ZHLN_PERF_SCREENSHOT  0/1     (default 1 — captures test_perf_output.ppm)
//
// Example:
//   ZHLN_PERF_WIDTH=1920 ZHLN_PERF_HEIGHT=1080 ZHLN_PERF_FRAMES=900 ./TestPerformance
//
// Expected baseline (RTX 3050 6 GB laptop, default 1280x720, validation OFF):
//   Roughly 40-70 FPS average over the measured window (12-25 ms/frame), with
//   p95 spikes up to ~2x the average while the per-frame TLAS rebuild and
//   shadow-map passes run. CPU (Jolt + ECS + fiber dispatch) lands around
//   8-15 ms/frame, so on this GPU the frame is roughly balanced between the
//   two. Validation ON typically halves the rate; 1080p drops the average to
//   roughly 25-45 FPS. Actual numbers depend on laptop TDP, driver, and
//   whether the UziProc.glb skeletal asset is present (adds skinning load).
//   Read the real breakdown from the printed ZAHLEN ENGINE PERFORMANCE
//   STRESS REPORT below.

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#if defined(ZHLN_PERF_WITH_EXTRAS)
#include <ALife/ALifeComponents.hpp>
#include <ALife/Simulator.hpp>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

// ============================================================================
// Error Codes
// ============================================================================

enum class PerfTestError : uint8_t {
    Success = 0,
    EngineCreateFailed[[= ZHLN::Reflect::Description("Engine/Vulkan initialization failed on this hardware.")]],
    SceneBuildFailed[[= ZHLN::Reflect::Description("Scene construction failed (asset factory, physics, or GPU resources).")]],
    TickFailed[[= ZHLN::Reflect::Description("Engine::Tick returned a non-OK status during the benchmark.")]],
    NoVisibleEntities[[= ZHLN::Reflect::Description("Culling reported zero visible entities on the final frame.")]],
    PhysicsDivergence[[= ZHLN::Reflect::Description("Physics bodies diverged (NaN/Inf positions or escaped world bounds).")]],
    FiberSystemStalled[[= ZHLN::Reflect::Description("The fiber dispatch system did not execute on every frame.")]],
    BlankFrame[[= ZHLN::Reflect::Description("The captured final frame is completely black (renderer produced no output).")]],
};

// ============================================================================
// Environment-driven Tunables
// ============================================================================

namespace {

[[nodiscard]] uint32_t PerfEnvU32(std::string_view name, uint32_t fallback) noexcept {
    const std::string key(name);
    if (const char* raw = std::getenv(key.c_str())) {
        char*         end = nullptr;
        const long    v   = std::strtol(raw, &end, 10);
        if (end != raw && v > 0) {
            return static_cast<uint32_t>(v);
        }
    }
    return fallback;
}

[[nodiscard]] float PerfEnvF32(std::string_view name, float fallback) noexcept {
    const std::string key(name);
    if (const char* raw = std::getenv(key.c_str())) {
        char* end = nullptr;
        const float v = std::strtof(raw, &end);
        if (end != raw && v > 0.0f) {
            return v;
        }
    }
    return fallback;
}

// Scales an integer workload size by the ZHLN_PERF_SCALE factor.
[[nodiscard]] uint32_t PerfScaled(uint32_t base, float scale) noexcept {
    const float v = static_cast<float>(base) * scale;
    return (v < 1.0f) ? 1u : static_cast<uint32_t>(std::lround(v));
}

struct PerfConfig {
    uint32_t width        = PerfEnvU32("ZHLN_PERF_WIDTH", 1280);
    uint32_t height       = PerfEnvU32("ZHLN_PERF_HEIGHT", 720);
    uint32_t warmupFrames = PerfEnvU32("ZHLN_PERF_WARMUP", 120);
    uint32_t measured     = PerfEnvU32("ZHLN_PERF_FRAMES", 600);
    bool     validation   = (PerfEnvU32("ZHLN_PERF_VALIDATION", 0) != 0);
    bool     screenshot   = (PerfEnvU32("ZHLN_PERF_SCREENSHOT", 1) != 0);
    float    scale        = PerfEnvF32("ZHLN_PERF_SCALE", 1.0f);

    // --- Scene population (all scaled by `scale`) ---
    uint32_t dynamicBoxes   = PerfScaled(320, scale);
    uint32_t staticBoxes    = PerfScaled(40, scale);
    uint32_t churnPerFrame  = PerfScaled(8, scale);
    uint32_t churnLifetime  = PerfScaled(75, scale);
    uint32_t pointLights    = PerfScaled(16, scale);
    uint32_t particleEmits  = PerfScaled(10, scale);
    uint32_t particlesEach  = PerfScaled(24576, scale);
    uint32_t meshEmits      = PerfScaled(4, scale);
    uint32_t meshParticles  = PerfScaled(256, scale);
    uint32_t decals         = PerfScaled(16, scale);
    uint32_t uziInstances   = PerfScaled(4, scale);
    uint32_t uiElements     = PerfScaled(40, scale);
    uint32_t lodEntities    = PerfScaled(8, scale);
    uint32_t dispatchCount  = PerfScaled(256, scale);
    uint32_t alifeCount     = PerfScaled(240, scale);
};

// ============================================================================
// Fiber-Dispatch Stress Component & System
//
// Attached to a large entity population; the system runs on the fiber task
// scheduler every frame (as part of the ECS update graph) and fans the work
// out with TaskSystem::ParallelFor, exercising the fiber pool, the counter
// sync primitives, and the graph's dependency dispatch.
// ============================================================================

struct PerfDispatchComponent {
    float    phase = 0.0f;
    float    value = 0.0f;
    uint32_t ticks = 0;
};

static void PerfFiberDispatchSystem(ZHLN::Engine& engine, float dt) {
    auto& reg  = engine.GetRegistry();
    auto  ents = reg.GetEntitiesWith<PerfDispatchComponent>();
    auto  comps = reg.GetRawArray<PerfDispatchComponent>();

    if (ents.empty()) {
        return;
    }

    ZHLN::TaskSystem::ParallelFor(static_cast<uint32_t>(ents.size()), 64, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) {
        for (uint32_t i = start; i < end; ++i) {
            PerfDispatchComponent& c = comps[i];
            c.phase += dt;
            c.value = std::sin(c.phase + 0.25f * static_cast<float>(chunkIdx)) * 2.0f;
            c.ticks += 1;
        }
    });
}

// ============================================================================
// Runtime Scene State
// ============================================================================

struct PerfScene {
    ZHLN::AssetID    crateMeshAsset = ZHLN::InvalidAssetID;
    ZHLN::MaterialID crateMatAssets[8] = {};
    uint32_t         crateMatCount = 0;
    JPH::ShapeRefC   crateShape = nullptr;

    ZHLN::Entity characterEntity = ZHLN::NullEntity;
    ZHLN::Entity characterBody   = ZHLN::NullEntity;
    ZHLN::Entity cameraEntity    = ZHLN::NullEntity;
    float        orbitAngle      = 0.0f;

    struct ChurnEntry {
        ZHLN::Entity entity;
        ZHLN::Entity body;
        uint32_t     dieFrame; // Frame at which the ECS entity is deferred-destroyed.
        bool         entityGone = false;
    };
    std::vector<ChurnEntry> churnPool;

#if defined(ZHLN_PERF_WITH_EXTRAS)
    std::unique_ptr<ZHLN::ALife::Simulator> alife;
    std::vector<ZHLN::Entity>               alifeEntities;
    std::vector<JPH::RVec3>                 alifeWaypoints;
    std::atomic<uint64_t>                   alifeEventCounter {0};
#endif
};

struct PerfCounters {
    uint32_t created     = 0;
    uint32_t destroyed   = 0;
    uint32_t ecbChurned  = 0;
    uint32_t explosions  = 0;
    uint32_t scriptCalls = 0;
    uint32_t audioEvents = 0;
    uint32_t queries     = 0;
};

// ============================================================================
// Scene Construction
// ============================================================================

namespace {

// Registers one shared crate mesh + a palette of shared crate materials.
// All dynamic/churned boxes reuse these assets so per-frame churn never
// triggers new GPU mesh/material/pipeline uploads.
void BuildSharedCrateAssets(ZHLN::RenderContext& rc, PerfScene& scene, const JPH::Vec4 palette[8], uint32_t count) {
    ZHLN::Mesh crateMesh = ZHLN::CreativeWorksFactory::CreateBoxMesh(rc, JPH::Vec3(0.3f, 0.3f, 0.3f), {0.8f, 0.8f, 0.8f, 1.0f});

    ZHLN::Material crateMat = ZHLN::CreativeWorksFactory::CreateBasicMaterial(rc).value_or(ZHLN::Material {});
    crateMat.roughnessFactor = 0.35f;
    crateMat.metallicFactor  = 0.15f;

    ZHLN::AssetID    meshAsset = ZHLN::HashAssetID("perf_crate_mesh");
    ZHLN::MaterialID matAssets[8] = {};

    rc.RegisterGPUMesh(meshAsset, crateMesh);
    for (uint32_t i = 0; i < count; ++i) {
        ZHLN::Material m = crateMat;
        m.baseColorFactor[0] = palette[i].GetX();
        m.baseColorFactor[1] = palette[i].GetY();
        m.baseColorFactor[2] = palette[i].GetZ();
        m.baseColorFactor[3] = 1.0f;

        matAssets[i] = ZHLN::HashAssetID("perf_crate_mat_" + std::to_string(i));
        rc.RegisterGPUMaterial(matAssets[i], m);
    }

    scene.crateMeshAsset = meshAsset;
    for (uint32_t i = 0; i < count; ++i) {
        scene.crateMatAssets[i] = matAssets[i];
    }
    scene.crateMatCount = count;
}

// One dynamic physics box + render entity sharing the crate assets.
void SpawnDynamicCrate(ZHLN::Engine& engine, ZHLN::AssetID meshAsset, ZHLN::MaterialID matAsset, const JPH::ShapeRefC& shape,
                       const JPH::RVec3& pos, const JPH::Quat& rot, bool addDispatch) {
    auto&       reg  = engine.GetRegistry();
    auto&       pc   = engine.GetPhysicsContext();
    const JPH::Vec3 posF(pos);
    const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(posF, rot, JPH::Vec3::sReplicate(1.0f));

    ZHLN::Entity body = pc.CreateRigidBody(shape, pos, rot, JPH::EMotionType::Dynamic, ZHLN::Layers::MOVING);
    ZHLN::Entity e = reg.Create(
        ZHLN::Components::TransformComponent {.position = posF, .rotation = rot},
        ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
        ZHLN::Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = 1.2f},
        ZHLN::Components::PBRComponent {.roughness = 0.35f, .metallic = 0.15f}
    );
    reg.Add(e, ZHLN::Components::PhysicsComponent {body});
    reg.Add(
        e,
        ZHLN::Components::PhysicsStateComponent {
            .currPosition = posF, .prevPosition = posF, .currRotation = rot, .prevRotation = rot
        }
    );
    if (addDispatch) {
        reg.Add(e, PerfDispatchComponent {});
    }
}

bool BuildPerfScene(ZHLN::Engine& engine, const PerfConfig& cfg, PerfScene& scene, std::mt19937& rng) {
    using namespace ZHLN;
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();
    auto& pc  = engine.GetPhysicsContext();

    const JPH::Vec4 cratePalette[8] = {
        {0.85f, 0.45f, 0.25f, 1.0f}, {0.30f, 0.55f, 0.85f, 1.0f}, {0.45f, 0.80f, 0.40f, 1.0f}, {0.90f, 0.80f, 0.30f, 1.0f},
        {0.75f, 0.35f, 0.75f, 1.0f}, {0.35f, 0.75f, 0.75f, 1.0f}, {0.85f, 0.60f, 0.30f, 1.0f}, {0.55f, 0.55f, 0.60f, 1.0f},
    };
    const uint32_t matCount = (cfg.dynamicBoxes > 0) ? 8u : 1u;
    BuildSharedCrateAssets(rc, scene, cratePalette, matCount);
    scene.crateShape = pc.GetOrCreateShape(Physics::ShapeType::Box, 0.3f, 0.3f, 0.3f);

    // --- Ground: physics plane + large visual floor ---
    const Entity ground = CreativeWorksFactory::CreatePlane(
        engine, 120.0f, {0.42f, 0.42f, 0.45f, 1.0f},
        CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = true, .isStaticPhysics = true}
    );
    if (!reg.IsAlive(ground)) {
        return false;
    }

    // --- Terrain (visual only, off to the side) ---
    const Entity terrain = CreativeWorksFactory::CreateTerrain(
        engine, 128, 280.0f, 35.0f, CreativeWorksFactory::TerrainType::Default,
        CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(160, 0, 0), .createPhysics = false}
    );
    if (!reg.IsAlive(terrain)) {
        return false;
    }

    // --- Static obstacle boxes (shared shapes, shared crate assets) ---
    const JPH::Vec3 staticSizes[5] = {JPH::Vec3(1.0f, 1.0f, 1.0f), JPH::Vec3(2.0f, 1.0f, 1.0f), JPH::Vec3(1.0f, 2.5f, 1.0f),
                                      JPH::Vec3(3.0f, 0.5f, 1.5f), JPH::Vec3(0.8f, 1.2f, 2.2f)};
    JPH::ShapeRefC staticShapes[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        staticShapes[i] = pc.GetOrCreateShape(Physics::ShapeType::Box, staticSizes[i].GetX(), staticSizes[i].GetY(), staticSizes[i].GetZ());
    }

    for (uint32_t i = 0; i < cfg.staticBoxes; ++i) {
        const uint32_t sizeIdx = rng() % 5;
        const float    ang     = static_cast<float>(rng() % 3600) * 0.1f;
        const float    rad     = 18.0f + static_cast<float>(rng() % 1000) * 0.06f; // 18..78 m ring
        const JPH::Vec3 pos    = JPH::Vec3(std::cos(ang) * rad, staticSizes[sizeIdx].GetY(), std::sin(ang) * rad);
        const JPH::Quat rot    = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), ang);
        const JPH::Vec3 scl    = JPH::Vec3::sReplicate(1.0f);
        const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(pos, rot, scl);

        const Entity body = pc.CreateRigidBody(staticShapes[sizeIdx], JPH::RVec3(pos), rot, JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
        const Entity e = reg.Create(
            ZHLN::Components::TransformComponent {.position = pos, .rotation = rot},
            ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
            ZHLN::Components::MeshComponent {
                .meshAsset = scene.crateMeshAsset, .materialAsset = scene.crateMatAssets[rng() % matCount],
                .cullRadius = 2.0f * staticSizes[sizeIdx].Length()
            },
            ZHLN::Components::PBRComponent {.roughness = 0.5f, .metallic = 0.1f}
        );
        reg.Add(e, ZHLN::Components::PhysicsComponent {body});
        reg.Add(e, PerfDispatchComponent {});
    }

    // --- Dynamic crate field (the physics churn pool baseline) ---
    for (uint32_t i = 0; i < cfg.dynamicBoxes; ++i) {
        const float    ang = static_cast<float>(rng() % 6283) * 0.01f;
        const float    rad = 2.0f + static_cast<float>(rng() % 1000) * 0.10f; // 2..12 m
        const JPH::RVec3 pos(static_cast<double>(std::cos(ang) * rad), 6.0 + static_cast<double>(rng() % 1000) * 0.014,
                             static_cast<double>(std::sin(ang) * rad));
        const JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), ang);
        SpawnDynamicCrate(engine, scene.crateMeshAsset, scene.crateMatAssets[rng() % matCount], scene.crateShape, pos, rot, i < cfg.dispatchCount);
    }

    // --- Character (dual-shape) driven in an orbit, followed by the target cam ---
    const Entity charBody = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0), Physics::DualShapeConfig {});
    const Entity charEnt  = reg.Create(
        ZHLN::Components::TransformComponent {.position = JPH::Vec3(0, 0, 0)},
        ZHLN::Components::WorldTransformComponent {},
        ZHLN::Components::PhysicsComponent {charBody},
        ZHLN::Components::PhysicsStateComponent {},
        ZHLN::Components::MovementComponent {.speed = 6.0f},
        PerfDispatchComponent {}
    );
    scene.characterEntity = charEnt;
    scene.characterBody   = charBody;

    // Retarget the main camera at the character (drops free-cam mode).
    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (camEnts.empty()) {
        return false;
    }
    scene.cameraEntity = camEnts[0];
    reg.Remove<Components::FreeCamTagComponent>(scene.cameraEntity);
    reg.Patch<Components::TargetCameraComponent>(scene.cameraEntity, [](auto& tc) {
        tc.distance         = 9.0f;
        tc.targetDistance   = 9.0f;
        tc.yaw              = -90.0f;
        tc.pitch            = -14.0f;
        tc.fov              = 50.0f;
        tc.targetFov        = 50.0f;
        tc.hasInitSmoothTarget = 1;
    });

    // --- Sun (directional) with cascaded shadow map ---
    const Entity sunEnt = reg.Create();
    reg.Add(sunEnt, Components::TransformComponent {.position = JPH::Vec3(0, 40, 0), .rotation = Math::EulerDegreesToQuat({50.0f, -35.0f, 0.0f})});
    reg.Add(sunEnt, Components::LightComponent {.type = LightType::Sun, .color = JPH::Vec3(1.0f, 0.98f, 0.94f), .intensity = 180.0f});

    // --- Point light ring (first two cast punctual shadows) ---
    for (uint32_t i = 0; i < cfg.pointLights; ++i) {
        const float ang = (360.0f / static_cast<float>(cfg.pointLights)) * static_cast<float>(i);
        const float rad = 6.0f + (i % 3) * 4.0f;
        const JPH::Vec3 pos(std::cos(ang * 0.01745f) * rad, 2.5f + (i % 4), std::sin(ang * 0.01745f) * rad);
        const JPH::Vec3 lightColor = (i % 4 == 0) ? JPH::Vec3(1.0f, 0.75f, 0.5f) : (i % 4 == 1) ? JPH::Vec3(0.5f, 0.8f, 1.0f) : (i % 4 == 2) ? JPH::Vec3(0.7f, 1.0f, 0.6f) : JPH::Vec3(1.0f, 1.0f, 1.0f);

        const Entity lightEnt = reg.Create();
        reg.Add(lightEnt, Components::TransformComponent {.position = pos});
        reg.Add(
            lightEnt,
            Components::LightComponent {
                .type = LightType::Point, .color = lightColor, .intensity = 140.0f, .radius = 0.25f, .range = 28.0f
            }
        );
    }

    // --- Particle emitters (GPU-simulated, heavy overdraw) ---
    for (uint32_t i = 0; i < cfg.particleEmits; ++i) {
        const float ang = (360.0f / static_cast<float>(cfg.particleEmits)) * static_cast<float>(i);
        const JPH::Vec3 pos(std::cos(ang * 0.01745f) * 5.0f, 0.4f, std::sin(ang * 0.01745f) * 5.0f);

        ParticleEmitterParams params {};
        params.spawnOrigin        = {pos.GetX(), pos.GetY(), pos.GetZ()};
        params.spawnBoxExtent     = {1.5f, 0.3f, 1.5f};
        params.initVelMin         = {0.5f, 1.0f, 0.5f};
        params.initVelMax         = {1.5f, 3.5f, 1.5f};
        params.lifetimeMin        = 1.2f;
        params.lifetimeMax        = 3.0f;
        params.startColor         = {(0.6f + 0.4f * (i % 3)) / 3.0f, (0.6f + 0.4f * ((i + 1) % 3)) / 3.0f, (0.6f + 0.4f * ((i + 2) % 3)) / 3.0f, 1.0f};
        params.endColor           = {params.startColor[0], params.startColor[1], params.startColor[2], 0.0f};
        params.startSize          = {0.12f, 0.12f};
        params.endSize            = {0.02f, 0.02f};
        params.turbulence         = {0.4f, 0.1f, 0.4f};
        params.turbulenceFreq     = 0.8f;
        params.spinSpeed          = 2.0f;
        params.blendMode          = (i % 2); // alternate alpha / additive
        params.alignment          = ParticleAlignment::CameraBillboard;

        const Entity e = reg.Create(
            Components::TransformComponent {.position = pos},
            Components::ParticleEmitterComponent {.params = params, .maxParticles = cfg.particlesEach, .active = true}
        );
        reg.Add(e, PerfDispatchComponent {});
    }

    // --- 3D mesh particle emitters ---
    for (uint32_t i = 0; i < cfg.meshEmits; ++i) {
        const JPH::Vec3 pos(8.0f, 1.0f + i, 8.0f + i);
        MeshParticleEmitterParams params {};
        params.spawnOrigin   = {pos.GetX(), pos.GetY(), pos.GetZ()};
        params.spawnBoxExtent = {2.0f, 0.5f, 2.0f};
        params.initVelMin    = {-3.0f, 1.0f, -3.0f};
        params.initVelMax    = {3.0f, 6.0f, 3.0f};
        params.lifetimeMin   = 1.0f;
        params.lifetimeMax   = 2.5f;
        params.scaleMin      = 0.05f;
        params.scaleMax      = 0.2f;

        const Entity e = reg.Create(
            Components::TransformComponent {.position = pos},
            Components::MeshParticleEmitterComponent {
                .meshAsset = scene.crateMeshAsset, .materialAsset = scene.crateMatAssets[i % matCount],
                .maxParticles = cfg.meshParticles, .active = true, .params = params
            }
        );
        reg.Add(e, PerfDispatchComponent {});
    }

    // --- Decals (procedural textures stacked on the floor) ---
    {
        std::vector<uint32_t> px(128 * 128);
        for (uint32_t y = 0; y < 128; ++y) {
            for (uint32_t x = 0; x < 128; ++x) {
                const float dx = static_cast<float>(x) / 128.0f - 0.5f;
                const float dy = static_cast<float>(y) / 128.0f - 0.5f;
                const float d  = std::sqrt(dx * dx + dy * dy) * 2.0f;
                const uint8_t r = static_cast<uint8_t>(std::clamp(255.0f * (1.0f - d), 0.0f, 255.0f));
                const uint8_t g = static_cast<uint8_t>(std::clamp(120.0f * (1.0f - d * 0.6f), 0.0f, 255.0f));
                px[y * 128 + x] = (0xFFu << 24) | (static_cast<uint32_t>(g) << 16) | (static_cast<uint32_t>(r) << 8) | 24u;
            }
        }
        const TextureHandle decalTex  = rc.CreateProceduralTexture("perf_decal", 128, 128, true, px.data());
        const TextureHandle decalNorm = rc.CreateProceduralTexture("perf_decal_norm", 128, 128, false, px.data());

        for (uint32_t i = 0; i < cfg.decals; ++i) {
            const float ang = static_cast<float>(i) * 0.7f;
            const float rad = 2.0f + (i % 5) * 2.0f;
            const JPH::Vec3 pos(std::cos(ang) * rad, 0.02f + (i % 4) * 0.005f, std::sin(ang) * rad);
            const JPH::Vec3 scl(1.6f, 1.0f, 1.6f);
            const JPH::Mat44 worldMat = Math::CreateTransform(pos, JPH::Quat::sIdentity(), scl);

            const Entity e = reg.Create(
                Components::TransformComponent {.position = pos, .scale = scl},
                Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                Components::DecalComponent {.albedoMap = decalTex, .normalMap = decalNorm, .roughness = 0.7f, .metallic = 0.0f}
            );
            reg.Add(e, PerfDispatchComponent {});
        }
    }

    // --- LOD objects scattered at increasing distance ---
    for (uint32_t i = 0; i < cfg.lodEntities; ++i) {
        const float dist   = 12.0f + static_cast<float>(i) * 6.0f;
        const float ang    = static_cast<float>(i) * 2.399f;
        const JPH::Vec3    pos(std::cos(ang) * dist, 0.5f, std::sin(ang) * dist);
        Components::LODComponent lod {};
        lod.levels[0].meshAsset = scene.crateMeshAsset;
        lod.levels[0].distance  = 0.0f;
        lod.levels[1].meshAsset = scene.crateMeshAsset;
        lod.levels[1].distance  = 20.0f;
        lod.count               = 2;

        const Entity e = reg.Create(
            Components::TransformComponent {.position = pos},
            Components::WorldTransformComponent {.world = Math::CreateTransform(pos, JPH::Quat::sIdentity(), JPH::Vec3::sReplicate(1.0f))},
            Components::MeshComponent {.meshAsset = scene.crateMeshAsset, .materialAsset = scene.crateMatAssets[i % matCount], .cullRadius = 1.5f},
            lod
        );
        reg.Add(e, PerfDispatchComponent {});
    }

    // --- Native ECS UI (panels + text, rendered by UIRenderSystem) ---
    {
        const Entity root = reg.Create(
            Components::UIRectComponent {.x = 12.0f, .y = 12.0f, .width = 340.0f, .height = 420.0f},
            Components::UIPanelComponent {.color = {0.08f, 0.11f, 0.16f, 0.92f}, .edgeWidth = 1.5f},
            Components::UIFlexComponent {.direction = FlexDirection::Column, .gapY = 8.0f}
        );

        for (uint32_t i = 0; i < cfg.uiElements; ++i) {
            const bool isText = (i % 4 == 3);
            if (isText) {
                const Entity t = reg.Create(
                    Components::UIRectComponent {.parentEntity = root, .width = 320.0f, .height = 26.0f},
                    Components::TextComponent {.text = String256("PERF " + std::to_string(i) + " FPS STRESS"), .scale = 1.0f, .color = {0.8f, 0.9f, 1.0f, 1.0f}}
                );
                (void) t;
            } else {
                const JPH::Vec4 panelColor = (i % 2 == 0) ? JPH::Vec4(0.13f, 0.18f, 0.26f, 0.95f) : JPH::Vec4(0.22f, 0.15f, 0.13f, 0.95f);
                const Entity p = reg.Create(
                    Components::UIRectComponent {.parentEntity = root, .width = 320.0f, .height = 26.0f},
                    Components::UIPanelComponent {.color = panelColor, .edgeWidth = 1.0f},
                    Components::UIButtonComponent {}
                );
                (void) p;
            }
        }
    }

    // --- Volumetric fog + scattering volumes (pipeline always runs) ---
    reg.Create(
        Components::VolumetricFogComponent {
            .density = 0.015f, .heightFalloff = 0.04f, .anisotropy = 0.55f,
            .scatteringColor = JPH::Vec3(0.91f, 0.95f, 1.0f), .noiseScale = 0.03f, .noiseSpeed = 0.6f, .noiseIntensity = 0.4f
        }
    );
    for (uint32_t i = 0; i < 2; ++i) {
        reg.Create(
            Components::VolumetricVolumeComponent {
                .type = (i == 0) ? Components::VolumetricVolumeType::Box : Components::VolumetricVolumeType::Sphere,
                .extents = JPH::Vec3(6.0f, 4.0f, 6.0f), .density = 0.12f, .color = JPH::Vec3(0.9f, 0.85f, 0.7f), .anisotropy = 0.4f
            },
            Components::TransformComponent {.position = JPH::Vec3(10.0f + i * 8.0f, 2.0f, -6.0f)}
        );
    }

    // --- Interaction sandbox: triggers, pickups, a container with an item ---
    {
        const Entity item = reg.Create(
            Components::ItemBaseComponent {.name = String64("PerfCell"), .id = 42, .icon = String64("cell")},
            Components::TransformComponent {.position = JPH::Vec3(3.0f, 0.3f, 3.0f)}
        );

        Entity containerEnt = ZHLN::NullEntity;
        for (uint32_t i = 0; i < 6; ++i) {
            const JPH::Vec3 pos(4.0f, 0.5f, -4.0f + static_cast<float>(i) * 1.5f);
            const Entity    e = reg.Create(
                Components::TransformComponent {.position = pos},
                Components::TriggerComponent {.radius = 1.5f},
                Components::PickupComponent {}
            );
            if (i == 0) {
                reg.Add(e, Components::ContainerComponent {});
                containerEnt = e;
            } else {
                reg.Add(e, Components::UsableComponent {});
            }
        }

        if (containerEnt != ZHLN::NullEntity) {
            if (auto* container = reg.Get<Components::ContainerComponent>(containerEnt)) {
                container->slots[0] = item;
                container->count    = 1;
            }
        }
    }

    // --- Audio: listener on the camera + loop synths ---
    reg.Add(scene.cameraEntity, Components::AudioListenerComponent {.isPrimary = true});
    {
        const AudioWaveformType waves[] = {AudioWaveformType::Sine, AudioWaveformType::Sawtooth, AudioWaveformType::Square, AudioWaveformType::Triangle};
        for (uint32_t i = 0; i < 4; ++i) {
            const Entity e = reg.Create(
                Components::TransformComponent {.position = JPH::Vec3(static_cast<float>(i) * 2.0f - 3.0f, 1.0f, 0.0f)},
                Components::LoopSynthComponent {
                    .waveType1 = waves[i], .waveType2 = waves[(i + 1) % 4], .filterType = AudioFilterType::LowPass,
                    .charge = 0.4f, .baseFreq = 40.0f + 25.0f * i, .filterFreq = 600.0f + 200.0f * i, .volume = 0.12f
                }
            );
            (void) e;
        }
    }

    // --- Post-process / shadow settings (full pipeline: SSGI + SSR + RTR + TAA) ---
    {
        auto gEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
        if (!gEnts.empty()) {
            if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(gEnts[0])) {
                pp->giMode       = 2; // SSGI
                pp->giSamples    = 8;
                pp->giIntensity  = 1.2f;
                pp->enableSSR    = 1;
                pp->enableRTR    = rc.RayTracingSupported() ? 1 : 0;
                pp->fullBright   = 0;
                pp->useLocalProbe = 1;
            }
            if (auto* sh = reg.Get<Components::ShadowSettingsComponent>(gEnts[0])) {
                sh->shadowWidth        = 200.0f;
                sh->shadowResolution   = 2048;
                sh->maxPunctualShadows = 2;
                sh->sunSize            = 0.05f;
            }
        }
        (void) rc.SetShadowResolution(2048);
    }

    // --- Skeletal animation: instantiate the real Uzi GLB when the asset is
    // --- available (Git-LFS check mirrors tests/extras conventions). Loaded
    // --- from memory so it works regardless of the test's working directory.
    {
        const std::string assetPath = std::string(ZHLN_TEST_SOURCE_DIR) + "/resources/assets/UziProc.glb";
        std::ifstream     stream(assetPath, std::ios::binary | std::ios::ate);
        const auto        fileSize = stream.tellg();
        stream.seekg(0, std::ios::beg);
        char magic[4] = {};
        stream.read(magic, sizeof(magic));

        if (stream.gcount() == 4 && std::string_view(magic, 4) == "glTF" && fileSize > 64) {
            std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

            std::vector<Entity> parts(1024);
            const uint32_t      spawned = CreativeWorksFactory::InstantiatePrefabFromMemory(
                engine, std::span<const uint8_t>(bytes), "UziProc.glb",
                CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(2.0, 0.0, 2.0), .createPhysics = false, .isStaticPhysics = true, .isAnimated = true
                },
                parts.data(), 1024
            );
            ZHLN::Println("    [Perf] Instantiated {} skeletal prefab part(s) from UziProc.glb (x{} copies)", spawned, cfg.uziInstances);

            // Spawn additional animated copies around the arena for skinning load.
            for (uint32_t copy = 1; copy < cfg.uziInstances && spawned > 0; ++copy) {
                const float ang = static_cast<float>(copy) * 1.5708f;
                CreativeWorksFactory::InstantiatePrefabFromMemory(
                    engine, std::span<const uint8_t>(bytes), "UziProc.glb",
                    CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(static_cast<double>(std::cos(ang) * 4.0f), 0.0, static_cast<double>(std::sin(ang) * 4.0f)),
                        .rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), ang),
                        .createPhysics = false, .isStaticPhysics = true, .isAnimated = true
                    },
                    parts.data(), 1024
                );
            }
        } else {
            ZHLN::Println("    [Perf] NOTE: UziProc.glb missing or an LFS pointer — skeletal animation portion skipped.");
        }
    }

    // --- ALife (first-party extras): creature population with parallel
    // --- think/move/interaction phases + faction combat churn. ---
#if defined(ZHLN_PERF_WITH_EXTRAS)
    reg.RegisterComponent<ALife::ALifeComponent>("ALifeComponent");

    ALife::SimConfig simCfg {};
    simCfg.max_factions = 8;
    simCfg.grid_width   = 100;
    simCfg.grid_height  = 100;
    simCfg.cell_size    = 50.0f;
    simCfg.default_tuning.time_factor     = 2.0f;
    simCfg.default_tuning.switch_distance = 30.0f;

    scene.alife = std::make_unique<ALife::Simulator>(simCfg);
    scene.alife->SetRelation(0, 1, -1.0f);
    scene.alife->SetRelation(2, 3, -1.0f);
    scene.alife->on_event = [&scene](ALife::Simulator&, const ALife::Event&) {
        scene.alifeEventCounter.fetch_add(1, std::memory_order_relaxed);
    };
    scene.alife->on_think = [](ALife::Simulator&, Entity) {
        // The per-tick think callback itself is the stress load.
    };
    scene.alife->on_interaction = [&reg](ALife::Simulator& sim, Entity e1, Entity e2) {
        sim.ResolveOfflineInteraction(reg, e1, e2);
    };

    // The ALife spatial grid only indexes the positive quadrant, so keep the
    // creature population (and its roaming waypoints) inside [0, 120] x [0, 120].
    for (uint32_t i = 0; i < cfg.alifeCount; ++i) {
        const JPH::RVec3 pos(static_cast<double>(rng() % 120), 0.0, static_cast<double>(rng() % 120));
        // self_entity is left as NullEntity; the ALife SpatialGrid caches the
        // real handle into it on first UpdateEntity.
        const Entity e = reg.Create(
            ALife::ALifeComponent {
                .position = pos, .state = ALife::State::Offline, .travel_speed = 5.0f + static_cast<float>(rng() % 700) * 0.01f,
                .faction_id = i % 4, .health = 100, .power = 10, .energy = 100
            },
            Components::TransformComponent {.position = JPH::Vec3(pos)},
            PerfDispatchComponent {}
        );
        scene.alifeEntities.push_back(e);
        scene.alifeWaypoints.push_back(pos);
    }
#else
    ZHLN::Println("    [Perf] NOTE: ZHLN_BUILD_EXTRAS off — ALife population skipped.");
#endif

    return true;
}

// ============================================================================
// Per-Frame Drive (churn, steering, events)
// ============================================================================

namespace {

void DriveFrame(ZHLN::Engine& engine, PerfScene& scene, PerfCounters& counters, const PerfConfig& cfg, uint32_t frame, std::mt19937& rng) {
    using namespace ZHLN;
    auto& reg   = engine.GetRegistry();
    auto& pc    = engine.GetPhysicsContext();
    auto& ecb   = engine.GetMainECB();
    auto& audio = engine.GetAudioContext();

    // ------------------------------------------------------------------
    // 1. ECS + physics churn: expire old churn entities (two-phase so the
    //    ECS entity is dead BEFORE its physics slot is freed — WriteBack
    //    must never read a stale slotToDense entry):
    //      phase 1 (frame dieFrame)    : defer the ECS destruction (ECB
    //                                    playback inside this frame's Tick)
    //      phase 2 (frame dieFrame + 1): destroy the Jolt body (FlushCommands
    //                                    inside this frame's Tick, entity gone)
    // ------------------------------------------------------------------
    for (auto it = scene.churnPool.begin(); it != scene.churnPool.end();) {
        if (!it->entityGone && frame >= it->dieFrame) {
            ecb.DestroyEntity(it->entity);
            it->entityGone = true;
            ++it;
        } else if (it->entityGone) {
            pc.DestroyBody(it->body);
            ++counters.destroyed;
            it = scene.churnPool.erase(it);
        } else {
            ++it;
        }
    }

    // Spawn fresh dynamic crates (shared assets + shared Jolt shape).
    for (uint32_t i = 0; i < cfg.churnPerFrame; ++i) {
        const float    ang = static_cast<float>(rng() % 6283) * 0.01f;
        const float    rad = 4.0f + static_cast<float>(rng() % 1000) * 0.08f;
        const JPH::RVec3 pos(static_cast<double>(std::cos(ang) * rad), 8.0 + static_cast<double>(rng() % 600) * 0.02,
                             static_cast<double>(std::sin(ang) * rad));
        const JPH::Quat  rot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), ang);

        const JPH::Vec3  posF(pos);
        const JPH::Mat44 worldMat = Math::CreateTransform(posF, rot, JPH::Vec3::sReplicate(1.0f));
        const Entity     body = pc.CreateRigidBody(scene.crateShape, pos, rot, JPH::EMotionType::Dynamic, Layers::MOVING);

        const Entity e = reg.Create(
            Components::TransformComponent {.position = posF, .rotation = rot},
            Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
            Components::MeshComponent {
                .meshAsset = scene.crateMeshAsset, .materialAsset = scene.crateMatAssets[rng() % scene.crateMatCount], .cullRadius = 1.2f
            },
            Components::PBRComponent {.roughness = 0.4f, .metallic = 0.1f},
            Components::PhysicsComponent {body},
            Components::PhysicsStateComponent {.currPosition = posF, .prevPosition = posF, .currRotation = rot, .prevRotation = rot},
            PerfDispatchComponent {}
        );
        scene.churnPool.push_back({e, body, frame + cfg.churnLifetime});
        ++counters.created;
    }

    // Pure-ECB same-frame churn (hammer the deferred create→destroy pipeline).
    if ((frame % 3) == 0) {
        for (uint32_t i = 0; i < 8; ++i) {
            const Entity pulse = ecb.CreateEntity(
                Components::TransformComponent {.position = JPH::Vec3(static_cast<float>(i), 1.0f, 0.0f)},
                Components::PBRComponent {.roughness = 0.5f, .metallic = 0.5f},
                PerfDispatchComponent {}
            );
            ecb.DestroyEntity(pulse);
            counters.ecbChurned += 2;
        }
    }

    // ------------------------------------------------------------------
    // 2. Character steering: orbit the arena center. MovementSystem (inside
    //    Tick) converts inputX/inputZ into the character velocity, so drive
    //    it through the component — direct SetCharacterVelocity calls would
    //    be overwritten by the system.
    // ------------------------------------------------------------------
    if (reg.IsAlive(scene.characterEntity)) {
        scene.orbitAngle += 0.01f; // ~0.6 rad/s at 60fps
        const JPH::Vec3 target(std::cos(scene.orbitAngle) * 12.0f, 0.0f, std::sin(scene.orbitAngle) * 12.0f);

        reg.Patch<Components::PhysicsStateComponent, Components::MovementComponent>(
            scene.characterEntity,
            [&](auto& state, auto& move) {
                const JPH::Vec3 delta = target - state.currPosition;
                const float     len   = delta.Length();
                if (len > 1e-3f) {
                    move.inputX = delta.GetX() / len;
                    move.inputZ = delta.GetZ() / len;
                } else {
                    move.inputX = 0.0f;
                    move.inputZ = 0.0f;
                }
            }
        );
    }

    // ------------------------------------------------------------------
    // 3. Periodic explosions + physics query gauntlet.
    // ------------------------------------------------------------------
    if ((frame % 45) == 0 && frame > 0) {
        const JPH::RVec3 center(static_cast<double>(rng() % 20) - 10.0, 1.5, static_cast<double>(rng() % 20) - 10.0);
        pc.AddRadialImpulse(center, 7.0f, 320.0f);
        ++counters.explosions;
    }

    if ((frame % 2) == 0) {
        JPH::Array<Physics::RaycastResult> rayHits;
        JPH::Array<Entity>                 overlaps;

        const Physics::RaycastResult downHit = pc.Raycast(JPH::RVec3(0, 20, 0), JPH::Vec3(0, -1, 0), 40.0f);
        ZHLN::Test::ExpectTrue(downHit.hasHit); // ground plane must always be hit
        pc.RaycastAll(JPH::RVec3(0, 15, 0), JPH::Vec3(0, -1, 0), 30.0f, rayHits);
        const Physics::RaycastPenetrationResult penHit = pc.RaycastPenetration(JPH::RVec3(0, 0.5, 0), JPH::Vec3(0, 1, 0), 5.0f);
        pc.OverlapSphere(JPH::RVec3(0, 1, 0), 15.0f, overlaps);
        pc.OverlapAABB(JPH::RVec3(-30, -1, -30), JPH::RVec3(30, 6, 30), overlaps);
        pc.QueryAABB(JPH::Vec3(-12, 0, -12), JPH::Vec3(12, 5, 12), overlaps);
        const JPH::ShapeRefC          capsule = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.4f, 0.5f);
        const Physics::ShapeCastResult scHit  = pc.Shapecast(capsule, JPH::RVec3(10, 8, 10), JPH::Quat::sIdentity(), JPH::Vec3(0, -1, 0), 12.0f);
        (void) penHit;
        (void) scHit;
        counters.queries += 7;
    }

    // ------------------------------------------------------------------
    // 4. Audio + scripting IPC (periodic, fire-and-forget).
    // ------------------------------------------------------------------
    if ((frame % 30) == 0 && frame > 0) {
        AudioEvent beep {};
        beep.type     = AudioEventType::ProceduralBeep;
        beep.position = JPH::Vec3(static_cast<float>(rng() % 40) - 20.0f, 2.0f, static_cast<float>(rng() % 40) - 20.0f);
        beep.volume   = 0.25f;
        beep.param1   = 300.0f + static_cast<float>(rng() % 700);
        beep.duration = 0.15f;
        audio.PostEvent(beep);
        ++counters.audioEvents;
    }

    if ((frame % 60) == 0 && frame > 0) {
        engine.GetScriptRunner().ExecuteString("if _zperf_counter then _zperf_counter = _zperf_counter + 1 else _zperf_counter = 1 end");
        ++counters.scriptCalls;
    }

    // ------------------------------------------------------------------
    // 5. ALife (extras): drive creature movement, then run the simulator
    //    (parallel phases: think/switch/move + spatial grid rebuild +
    //    offline faction interactions).
    // ------------------------------------------------------------------
#if defined(ZHLN_PERF_WITH_EXTRAS)
    if (scene.alife != nullptr && !scene.alifeEntities.empty()) {
        auto  ents  = reg.GetEntitiesWith<ALife::ALifeComponent>();
        auto  comps = reg.GetRawArray<ALife::ALifeComponent>();
        auto& wps   = scene.alifeWaypoints;

        TaskSystem::ParallelFor(static_cast<uint32_t>(ents.size()), 128, [&](uint32_t start, uint32_t end, uint32_t) {
            thread_local std::mt19937 localRng(0xC0FFEE);
            for (uint32_t i = start; i < end && i < wps.size(); ++i) {
                ALife::ALifeComponent& c = comps[i];
                if (c.state == ALife::State::Dead) {
                    continue;
                }

                // Respawn dead creatures (population churn).
                if (c.health <= 0) {
                    c.state       = ALife::State::Offline;
                    c.health      = 100;
                    c.is_fleeing  = false;
                    c.is_looted   = false;
                    c.wait_time   = 0;
                    c.position    = JPH::RVec3(static_cast<double>(localRng() % 120), 0.0, static_cast<double>(localRng() % 120));
                    wps[i]        = c.position;
                }

                // Steer toward the current waypoint (sim time factor 2.0x).
                JPH::RVec3   delta  = wps[i] - c.position;
                const double dist2  = delta.LengthSq();
                if (dist2 < 4.0) { // arrived: pick a new waypoint
                    wps[i] = JPH::RVec3(static_cast<double>(localRng() % 120), 0.0, static_cast<double>(localRng() % 120));
                    continue;
                }
                const double dist = std::sqrt(dist2);
                const double step = static_cast<double>(c.travel_speed) * 0.033333;
                if (step >= dist) {
                    c.position = wps[i];
                } else {
                    c.position += delta * (step / dist);
                }
            }
        });

        scene.alife->Update(engine, 0.016666f, JPH::RVec3(engine.GetCamera().position));
    }
#endif
}

} // namespace

// ============================================================================
// Reporting
// ============================================================================

namespace {

struct PerfSummary {
    double avgMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
};

[[nodiscard]] PerfSummary SummarizeFrames(const std::vector<double>& frameMs) {
    PerfSummary s {};
    if (frameMs.empty()) {
        return s;
    }

    std::vector<double> sorted(frameMs);
    std::sort(sorted.begin(), sorted.end());

    double total = 0.0;
    for (double ms: frameMs) {
        total += ms;
    }
    s.avgMs = total / static_cast<double>(frameMs.size());
    s.minMs = sorted.front();
    s.maxMs = sorted.back();
    s.p50Ms = sorted[(sorted.size() * 50) / 100];
    s.p95Ms = sorted[(sorted.size() * 95) / 100];
    return s;
}

// ZHLN::Println supports "{}" placeholders without precision specs, so all
// floating point values are pre-rounded before being handed over.
[[nodiscard]] inline double Round1(double v) noexcept {
    return std::floor(v * 10.0 + 0.5) / 10.0;
}

[[nodiscard]] inline double Round3(double v) noexcept {
    return std::floor(v * 1000.0 + 0.5) / 1000.0;
}

void PrintPerfReport(const ZHLN::RenderContext& rc, const PerfConfig& cfg, const PerfSummary& s, const PerfCounters& counters,
                     const ZHLN::ECS::Registry& reg, uint32_t measured, uint64_t alifeEvents) {
    const double avgFps = (s.avgMs > 0.0) ? (1000.0 / s.avgMs) : 0.0;
    const double minFps = (s.maxMs > 0.0) ? (1000.0 / s.maxMs) : 0.0;
    const double p50Fps = (s.p50Ms > 0.0) ? (1000.0 / s.p50Ms) : 0.0;
    const double p95Fps = (s.p95Ms > 0.0) ? (1000.0 / s.p95Ms) : 0.0;

    ZHLN::Println("");
    ZHLN::Println("{}============================================================{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);
    ZHLN::Println("{}  ZAHLEN ENGINE PERFORMANCE STRESS REPORT{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);
    ZHLN::Println("{}============================================================{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);
    ZHLN::Println("GPU: {}", rc.GetGPUName());
    ZHLN::Println("Renderer: {}", rc.GetRendererName());
    ZHLN::Println("Resolution: {}x{} | Validation: {} | RayTracing: {} | MeshShading: {}", cfg.width, cfg.height,
                  (cfg.validation ? "ON" : "OFF"), (rc.RayTracingSupported() ? "ON" : "OFF"), (rc.MeshShadingSupported() ? "ON" : "OFF"));
    ZHLN::Println("Workload scale: {}x | Warmup: {} frames (excluded) | Measured: {} frames", cfg.scale, cfg.warmupFrames, measured);
    ZHLN::Println("");
    ZHLN::Println("{}--- Frame rate (measured window) ---{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);
    ZHLN::Println("  avg: {} FPS  ({} ms/frame)", Round1(avgFps), Round3(s.avgMs));
    ZHLN::Println("  min: {} FPS | p50: {} FPS | p95: {} FPS ({} ms)", Round1(minFps), Round1(p50Fps), Round1(p95Fps), Round3(s.p95Ms));
    ZHLN::Println("  max frame time: {} ms", Round3(s.maxMs));
    ZHLN::Println("");
    ZHLN::Println("{}--- Subsystem activity over measured window ---{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);
    ZHLN::Println("  ECS: {} entities alive | churn created {} / destroyed {} | same-frame ECB ops {}",
                  reg.GetEntitiesWith<ZHLN::Components::TransformComponent>().size(), counters.created, counters.destroyed, counters.ecbChurned);
    ZHLN::Println("  Physics: {} explosion events | {} query batches", counters.explosions, counters.queries);
    ZHLN::Println("  Audio one-shots: {} | Script IPC calls: {} | ALife events: {}", counters.audioEvents, counters.scriptCalls, alifeEvents);
    ZHLN::Println("  Culling: {} objects | {} culled | {} tris total / {} rendered", ZHLN::CullingStats::TotalObjects, ZHLN::CullingStats::CulledObjects,
                  ZHLN::CullingStats::TotalTriangles, ZHLN::CullingStats::RenderedTriangles);
    ZHLN::Println("");
    ZHLN::Println("{}--- CPU profile (rolling average per frame, top contributors) ---{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);

    struct TopMetric {
        const char* name;
        float       avgMs;
    };
    std::vector<TopMetric> tops;
    ZHLN::CPUProfiler::IterateMetrics(
        [](const char* name, float, float avgMS, const float*, size_t, void* userData) {
            auto* list = static_cast<std::vector<TopMetric>*>(userData);
            list->push_back({name, avgMS});
        },
        &tops
    );
    std::sort(tops.begin(), tops.end(), [](const TopMetric& a, const TopMetric& b) { return a.avgMs > b.avgMs; });
    for (size_t i = 0; i < tops.size() && i < 10; ++i) {
        std::string name(tops[i].name);
        if (name.size() > 46) {
            name = name.substr(0, 45) + "~";
        }
        ZHLN::Println("  {:<46s} {} ms", name, Round3(static_cast<double>(tops[i].avgMs)));
    }
    ZHLN::Println("");
    ZHLN::Println("  (Compare against the RTX 3050 6GB baseline noted in the source header;");
    ZHLN::Println("   ZHLN_PERF_WIDTH/HEIGHT/FRAMES/SCALE env vars re-tune the workload.)");
    ZHLN::Println("{}============================================================{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);
    ZHLN::Println("");
}

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct PerformanceTestSuite {
    PerformanceTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);

        // The GPU stress test below runs for minutes; lift the framework's
        // 15-second per-method alarm for the lifetime of this suite.
        ZHLN::Test::SetGlobalTimeoutSeconds(3600);
    }

    ~PerformanceTestSuite() {
        ZHLN::Test::SetGlobalTimeoutSeconds(15);
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // ==================================================================
        // CPU-ONLY microbenchmark: ECS churn + fiber dispatch, no Vulkan.
        // ==================================================================
        std::expected<void, ZHLN::Error> cpu_fiber_ecs_dispatch_microbenchmark() {
            using namespace ZHLN;
            using namespace ZHLN::ECS;

            Registry reg;
            reg.RegisterComponents<Components::TransformComponent, Components::PBRComponent, Components::NameComponent>();
            reg.RegisterComponent<PerfDispatchComponent>("PerfDispatchComponent");

            const uint32_t N = 20000;
            using Clock = std::chrono::steady_clock;
            auto t0       = Clock::now();

            // 1. Bulk creation
            for (uint32_t i = 0; i < N; ++i) {
                reg.Create(
                    Components::TransformComponent {.position = JPH::Vec3(static_cast<float>(i % 64), 0.0f, static_cast<float>(i % 32))},
                    Components::PBRComponent {.roughness = 0.5f, .metallic = 0.1f},
                    Components::NameComponent {.name = String64("micro_" + std::to_string(i))}
                );
            }
            auto t1 = Clock::now();
            ZHLN::Test::ExpectEq(reg.GetEntitiesWith<Components::PBRComponent>().size(), static_cast<size_t>(N));

            // 2. Raw-array pass + Patch combinator
            auto  pbrs = reg.GetRawArray<Components::PBRComponent>();
            float acc  = 0.0f;
            for (auto& p: pbrs) {
                acc += p.roughness + p.metallic;
            }
            ZHLN::Test::ExpectTrue(acc > 0.0f);
            reg.Patch<Components::PBRComponent>(reg.GetEntitiesWith<Components::PBRComponent>()[0], [](auto& p) { p.roughness = 0.9f; });
            auto t2 = Clock::now();

            // 3. Component add/remove churn (5k cycles)
            const auto ents = reg.GetEntitiesWith<Components::PBRComponent>();
            for (uint32_t i = 0; i < 5000; ++i) {
                const Entity e = ents[i % N];
                reg.Remove<Components::NameComponent>(e);
                reg.Add(e, Components::NameComponent {.name = String64("churn")});
            }
            auto t3 = Clock::now();

            // 4. Deferred ECB: 5k create+destroy in one playback
            {
                EntityCommandBuffer ecb(reg);
                for (uint32_t i = 0; i < 5000; ++i) {
                    const Entity e = ecb.CreateEntity(Components::PBRComponent {.roughness = 0.5f, .metallic = 0.5f});
                    ecb.DestroyEntity(e);
                }
                ecb.Playback();
            }
            auto t4 = Clock::now();

            // 5. Fiber fan-out: 200k element ParallelFor + 2k dispatch round trips
            {
                std::vector<float> data(200000, 1.0f);
                ZHLN::TaskSystem::ParallelFor(static_cast<uint32_t>(data.size()), 512, [&](uint32_t start, uint32_t end, uint32_t) {
                    for (uint32_t i = start; i < end; ++i) {
                        data[i] = std::sin(data[i] + 0.001f) * 0.999f + 0.001f;
                    }
                });
                ZHLN::Test::ExpectTrue(data[0] > 0.0f);

                auto jobFn = [](void* arg) {
                    auto* acc = static_cast<std::atomic<float>*>(arg);
                    acc->fetch_add(1.0f, std::memory_order_relaxed);
                };
                std::atomic<float> acc {0.0f};
                for (uint32_t i = 0; i < 2000; ++i) {
                    ZHLN::TaskSystem::Task tasks[4] = {{jobFn, &acc}, {jobFn, &acc}, {jobFn, &acc}, {jobFn, &acc}};
                    ZHLN::TaskSystem::Counter sync;
                    ZHLN::TaskSystem::Dispatch(std::span(tasks, 4), &sync);
                    ZHLN::TaskSystem::Wait(&sync);
                }
                ZHLN::Test::ExpectEq(acc.load(std::memory_order_relaxed), 8000.0f);
            }
            auto t5 = Clock::now();

            // 6. Bulk destroy (generation recycling). Copy the handles first:
            //    Destroy() compacts the dense arrays, which would invalidate
            //    the span GetEntitiesWith returned.
            const auto  all       = reg.GetEntitiesWith<Components::PBRComponent>();
            std::vector<Entity> toDestroy(all.begin(), all.end());
            for (Entity e: toDestroy) {
                reg.Destroy(e);
            }
            auto t6 = Clock::now();
            ZHLN::Test::ExpectEq(reg.GetEntitiesWith<Components::PBRComponent>().size(), 0u);

            const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const double createMS = ms(t0, t1);
            ZHLN::Println(
                "    [CPU Micro] create {} ms ({} ent/ms) | pass+patch {} ms | churn {} ms | ecb {} ms | fibers {} ms | destroy {} ms",
                Round1(createMS), (createMS > 0.0) ? static_cast<uint32_t>(N / createMS) : 0u, Round1(ms(t1, t2)), Round1(ms(t2, t3)), Round1(ms(t3, t4)),
                Round1(ms(t4, t5)), Round1(ms(t5, t6))
            );

            const double totalMS = ms(t0, t6);
            ZHLN::Test::ExpectTrue(totalMS < 20000.0);
            return {};
        }

        // ==================================================================
        // THE full-engine GPU stress benchmark.
        // ==================================================================
        std::expected<void, ZHLN::Error> full_engine_gpu_stress_benchmark() {
            using namespace ZHLN;

            const PerfConfig cfg {};
            ZHLN::Println("    [Perf] Config: {}x{} | warmup {} | measured {} | scale {}x | validation {}", cfg.width, cfg.height, cfg.warmupFrames,
                          cfg.measured, cfg.scale, (cfg.validation ? "ON" : "OFF"));

            // 1. Engine (headless, no window server required).
            const EngineConfig engineCfg {
                .physics = {
                    .maxBodies = 4096, .maxBodyPairs = 16384, .maxContactConstraints = 16384, .tempAllocatorSize = 64 * 1024 * 1024
                },
                .render = {
                    .appName = "Zahlen Perf Stress", .width = cfg.width, .height = cfg.height, .vsync = false, .fullscreen = false,
                    .validationMode = cfg.validation ? ValidationMode::On : ValidationMode::Off, .headless = true
                }
            };

            auto engineRes = Engine::Create(engineCfg);
            const auto checkEngine = ZHLN::Test::AssertTrue(engineRes.has_value());
            if (!checkEngine) {
                return std::unexpected(PerfTestError::EngineCreateFailed);
            }
            const auto engine = std::move(engineRes.value());

            ZHLN::DefaultPreset::SetDisabled(true); // No fallback scene / popup.

            const bool sceneOK = engine->InitializeDefaultScene();
            const auto checkSceneInit = ZHLN::Test::AssertTrue(sceneOK);
            if (!checkSceneInit) {
                return std::unexpected(PerfTestError::SceneBuildFailed);
            }

            // Register the stress component and add the fiber dispatch system
            // to the update graph (re-compile after the initial graph build).
            auto& reg  = engine->GetRegistry();
            reg.RegisterComponent<PerfDispatchComponent>("PerfDispatchComponent");
            engine->GetUpdateGraph().AddSystem({
                .update_func = PerfFiberDispatchSystem, .name = "PerfFiberDispatchSystem",
                .access_pattern = {ECS::Write<PerfDispatchComponent>()}, .enabled = true
            });
            engine->GetUpdateGraph().Compile();

            // 2. Build the kitchen-sink scene (includes the ALife population
            //    when ZHLN_BUILD_EXTRAS is on).
            PerfScene    scene {};
            std::mt19937 rng(0x5EED);
            const bool   built = BuildPerfScene(*engine, cfg, scene, rng);
            const auto checkBuilt = ZHLN::Test::AssertTrue(built);
            if (!checkBuilt) {
                return std::unexpected(PerfTestError::SceneBuildFailed);
            }

            auto& rc = engine->GetRenderContext();
            ZHLN::Println("    [Perf] Scene ready: {} dynamic crates | {} static | {} point lights | {} particle emitters ({} each) | {} decals",
                          cfg.dynamicBoxes, cfg.staticBoxes, cfg.pointLights, cfg.particleEmits, cfg.particlesEach, cfg.decals);

            // 3. Warmup (shader compile, TLAS warm, pipeline cache fills).
            constexpr float kDt = 1.0f / 60.0f;
            {
                PerfCounters  warmupCounters {};
                std::mt19937  warmRng(0x1111);
                for (uint32_t frame = 0; frame < cfg.warmupFrames; ++frame) {
                    engine->ProcessEvents();
                    DriveFrame(*engine, scene, warmupCounters, cfg, frame, warmRng);
                    if (engine->Tick(kDt, GameplayDriver::Cpp) != GameplayStatus::OK) {
                        return std::unexpected(PerfTestError::TickFailed);
                    }
                }
            }

            // 4. Measured window.
            std::vector<double> frameMs;
            frameMs.reserve(cfg.measured);
            PerfCounters counters {};
            {
                std::mt19937 measureRng(0x2222);
                for (uint32_t frame = 0; frame < cfg.measured; ++frame) {
                    const auto t0 = std::chrono::steady_clock::now();
                    engine->ProcessEvents();
                    DriveFrame(*engine, scene, counters, cfg, frame, measureRng);
                    if (engine->Tick(kDt, GameplayDriver::Cpp) != GameplayStatus::OK) {
                        return std::unexpected(PerfTestError::TickFailed);
                    }
                    const auto t1 = std::chrono::steady_clock::now();
                    frameMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                }
            }

            // 5. Assertions.
            ZHLN::Test::ExpectEq(frameMs.size(), static_cast<size_t>(cfg.measured));
            ZHLN::Test::ExpectTrue(engine->GetCurrentFrame() >= static_cast<uint64_t>(cfg.warmupFrames + cfg.measured));

            // Culling must have found visible geometry on the final frame.
            ZHLN::Test::ExpectTrue(!engine->GetVisibleEntities().empty());
            if (engine->GetVisibleEntities().empty()) {
                return std::unexpected(PerfTestError::NoVisibleEntities);
            }

            // Physics must stay finite and inside world bounds.
            {
                auto&        pc      = engine->GetPhysicsContext();
                const auto   posView = pc.GetPositionBuffer();
                const auto*  posData = static_cast<const JPH::Real*>(posView.buf);
                const size_t bodyCount = posView.shape[0]; // total bodies, from the BufferView

                bool finite = (posData != nullptr);
                for (size_t d = 0; finite && d < bodyCount; ++d) {
                    for (int k = 0; k < 3; ++k) {
                        const float v = static_cast<float>(posData[d * 4 + k]);
                        if (!std::isfinite(v) || std::abs(v) > 2000.0f) {
                            finite = false;
                            break;
                        }
                    }
                }
                ZHLN::Test::ExpectTrue(finite);
                if (!finite) {
                    return std::unexpected(PerfTestError::PhysicsDivergence);
                }
            }

            // The fiber system must have ticked on every frame.
            {
                const auto dispatchComps = reg.GetRawArray<PerfDispatchComponent>();
                const auto dispatchEnts  = reg.GetEntitiesWith<PerfDispatchComponent>();
                bool       stalled       = dispatchEnts.empty();
                for (size_t i = 0; !stalled && i < dispatchEnts.size(); i += 16) {
                    if (dispatchComps[i].ticks < cfg.measured) {
                        stalled = true;
                    }
                }
                ZHLN::Test::ExpectFalse(stalled);
                if (stalled) {
                    return std::unexpected(PerfTestError::FiberSystemStalled);
                }
            }

            // 6. Screenshot sanity (non-blank output).
            if (cfg.screenshot) {
                const std::string ppmPath = "test_perf_output.ppm";
                const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
                const auto checkCapture = ZHLN::Test::AssertTrue(captureRes.has_value());
                if (!checkCapture) {
                    return std::unexpected(PerfTestError::BlankFrame);
                }

                std::ifstream ppm(ppmPath, std::ios::binary);
                std::string   header;
                int           width    = 0;
                int           height   = 0;
                int           maxColor = 0;
                ppm >> header >> width >> height >> maxColor;
                ppm.get();
                std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
                ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

                uint64_t nonBlack = 0;
                for (size_t i = 0; i < pixels.size(); i += 3) {
                    if (pixels[i] > 8 || pixels[i + 1] > 8 || pixels[i + 2] > 8) {
                        nonBlack++;
                    }
                }
                const double fraction = static_cast<double>(nonBlack) / static_cast<double>(width * height);
                ZHLN::Println("    [Perf] Screenshot: {}% non-black pixels ({})", Round1(fraction * 100.0), ppmPath);
                ZHLN::Test::ExpectTrue(fraction > 0.002);
                if (fraction <= 0.002) {
                    return std::unexpected(PerfTestError::BlankFrame);
                }
            }

            // 7. Report.
#if defined(ZHLN_PERF_WITH_EXTRAS)
            const uint64_t alifeEvents = (scene.alife != nullptr) ? scene.alifeEventCounter.load(std::memory_order_relaxed) : 0;
#else
            const uint64_t alifeEvents = 0;
#endif
            PrintPerfReport(rc, cfg, SummarizeFrames(frameMs), counters, reg, cfg.measured, alifeEvents);

            return {};
        }
    };
};

// ============================================================================
// Main
// ============================================================================

int main() {
    return ZHLN::Test::Runner::Run<PerformanceTestSuite>();
}

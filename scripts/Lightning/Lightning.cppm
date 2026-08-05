// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// --- Global Module Fragment: Engine & Physics Headers ---
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module ZHLN.Lightning;

export namespace ZHLN {

// ============================================================================
// 1. DATA TYPES & CONFIGURATION
// ============================================================================

enum class LightningPhase : uint8_t { Idle, SteppedLeader, ReturnStroke, Dissipating };

struct LightningConfig {
    float eta               = 2.0f;  // Branching factor / probability
    float peakCurrentKA     = 45.0f; // Peak current in kA
    float timeDilation      = 1.0f;  // Slow-motion factor
    bool  positivePolarity  = false; // Polarity (+CG / -CG)
    float ribbonWidth       = 0.8f;  // Width of the glowing 3D lightning trunk
    int   subdivisions      = 5;     // 2^5 = 32 segments on main trunk + branches
    float soundVolume       = 10.0f;
    float emissiveIntensity = 8000.0f;
};

struct LightningSegment {
    JPH::Vec3 start;
    JPH::Vec3 end;
    float     width;
    float     branchLevel;
};

// ============================================================================
// 2. ECS COMPONENT (Pure DOD Data Storage)
// ============================================================================

struct LightningComponent {
    LightningConfig config {};
    LightningPhase  phase = LightningPhase::Idle;

    float realTime       = 0.0f;
    float phaseTime      = 0.0f;
    float currentKA      = 0.0f;
    float flashLuminance = 0.0f;

    JPH::Vec3 cloudOrigin  = JPH::Vec3::sZero();
    JPH::Vec3 groundTarget = JPH::Vec3::sZero();

    std::vector<LightningSegment> segments {};

    // GPU Resource Handles
    BufferHandle vboPos          = BufferHandle::Invalid;
    BufferHandle vboAttr         = BufferHandle::Invalid;
    AssetID      meshAssetId     = InvalidAssetID;
    MaterialID   matAssetId      = InvalidMaterialID;
    uint32_t     maxVertices     = 0;
    uint32_t     visibleVertices = 0;

    // Sub-entities for point-light flashes
    Entity flashLightEntity  = NullEntity;
    Entity impactLightEntity = NullEntity;

    // Base exposure cached at spawn to prevent overlapping flash accumulation
    float baseAmbientExposure = 4.5f;

    // Automated ECS RAII Cleanup: Frees GPU VBOs on Entity Destruction
    static void OnDestroy(LightningComponent* c) noexcept {
        if (auto* engine = GetEngineContext()) {
            auto& rc = engine->GetRenderContext();

            if (c->vboPos != BufferHandle::Invalid) {
                rc.DestroyBuffer(c->vboPos);
                rc.DestroyBuffer(c->vboAttr);
                c->vboPos  = BufferHandle::Invalid;
                c->vboAttr = BufferHandle::Invalid;
            }
        }
    }
};

} // namespace ZHLN

// ============================================================================
// 3. FRACTAL MESH GENERATOR
// ============================================================================

export namespace ZHLN::LightningGenerator {

struct GeneratedRibbon {
    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    uint32_t                      maxVertices = 0;
};

inline std::vector<LightningSegment>
    GenerateFractalSegments(JPH::Vec3 start, JPH::Vec3 end, float startWidth, const LightningConfig& config, std::mt19937& rng) {
    std::vector<LightningSegment> queue;
    queue.push_back({.start = start, .end = end, .width = startWidth, .branchLevel = 0.0f});

    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> prob(0.0f, 1.0f);

    for (int step = 0; step < config.subdivisions; ++step) {
        std::vector<LightningSegment> nextQueue;
        float                         scale = std::pow(0.55f, static_cast<float>(step)) * (end - start).Length() * 0.18f;

        for (const auto& seg: queue) {
            JPH::Vec3 mid = (seg.start + seg.end) * 0.5f;

            JPH::Vec3 dir = (seg.end - seg.start);
            float     len = dir.Length();
            if (len < 0.1f) {
                nextQueue.push_back(seg);
                continue;
            }
            dir /= len;

            JPH::Vec3 side = dir.Cross(JPH::Vec3::sAxisY());
            if (side.LengthSq() < 1e-4f) {
                side = JPH::Vec3::sAxisX();
            } else {
                side = side.Normalized();
            }
            JPH::Vec3 up = dir.Cross(side).Normalized();

            JPH::Vec3 offset      = (side * dist(rng) + up * dist(rng)) * scale;
            JPH::Vec3 jitteredMid = mid + offset;

            nextQueue.push_back({.start = seg.start, .end = jitteredMid, .width = seg.width, .branchLevel = seg.branchLevel});
            nextQueue.push_back({.start = jitteredMid, .end = seg.end, .width = seg.width, .branchLevel = seg.branchLevel});

            float branchProb = (0.35f * config.eta / 2.0f) / (1.0f + seg.branchLevel * 0.8f);
            if (prob(rng) < branchProb) {
                JPH::Vec3 branchDir = (dir + (side * dist(rng) + up * dist(rng)) * 0.7f).Normalized();
                float     branchLen = len * (0.4f + prob(rng) * 0.3f);
                JPH::Vec3 branchEnd = jitteredMid + branchDir * branchLen;

                nextQueue.push_back({.start = jitteredMid, .end = branchEnd, .width = seg.width * 0.5f, .branchLevel = seg.branchLevel + 1.0f});
            }
        }
        queue = std::move(nextQueue);
    }
    return queue;
}

inline GeneratedRibbon BuildCameraFacingRibbon(std::span<const LightningSegment> segments, JPH::Vec3 cameraPos) {
    GeneratedRibbon ribbon;
    ribbon.positions.reserve(segments.size() * 6);
    ribbon.attributes.reserve(segments.size() * 6);

    Packed1010102 n = Math::PackNormal(0, 1, 0);
    Packed1010102 t = Math::PackNormal(1, 0, 0, 1);
    PackedRGBA8   c = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);

    for (const auto& seg: segments) {
        JPH::Vec3 p0 = seg.start;
        JPH::Vec3 p1 = seg.end;

        JPH::Vec3 dir = (p1 - p0);
        float     len = dir.Length();
        if (len < 1e-4f) {
            continue;
        }
        dir /= len;

        JPH::Vec3 toCam = (cameraPos - p0).Normalized();
        JPH::Vec3 side  = dir.Cross(toCam);
        if (side.LengthSq() < 1e-4f) {
            side = JPH::Vec3::sAxisX();
        } else {
            side = side.Normalized();
        }

        float width = seg.width;

        JPH::Vec3 v0 = p0 - side * width;
        JPH::Vec3 v1 = p0 + side * width;
        JPH::Vec3 v2 = p1 - side * width;
        JPH::Vec3 v3 = p1 + side * width;

        ribbon.positions.push_back({{v0.GetX(), v0.GetY(), v0.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 0), .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});

        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
        ribbon.positions.push_back({{v3.GetX(), v3.GetY(), v3.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 1), .color = c});
    }

    ribbon.maxVertices = static_cast<uint32_t>(ribbon.positions.size());
    return ribbon;
}

inline float EvaluateHeidler(float tUs, float i0, float t1, float t2) noexcept {
    if (tUs <= 0.0f || i0 <= 0.0f) {
        return 0.0f;
    }
    float x  = (tUs / t1) * (tUs / t1);
    float ec = std::exp(-(t1 / t2) * std::sqrt(2.0f * t2 / t1));
    return (i0 / ec) * (x / (1.0f + x)) * std::exp(-tUs / t2);
}

} // namespace ZHLN::LightningGenerator

// ============================================================================
// 4. ECS SYSTEM & API
// ============================================================================

export namespace ZHLN::Lightning {

/**
 * @brief Spawns a procedural 3D branching lightning strike as an ECS entity.
 */
Entity Spawn(Engine& engine, JPH::RVec3Arg cloudPos, JPH::RVec3Arg groundPos, const LightningConfig& cfg = {}) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    Entity             boltEntity = reg.Create();
    LightningComponent boltComp;
    boltComp.config = cfg;

    // --- FIX FOR AMBIENCE STACKING BUG ---
    // If another bolt is already active in the world, inherit ITS un-flashed baseline.
    // Otherwise, read the clean baseline exposure from the global settings component.
    float baseExposure = 4.5f;
    auto  existingEnts = reg.GetEntitiesWith<LightningComponent>();
    if (!existingEnts.empty()) {
        if (auto* existingComp = reg.Get<LightningComponent>(existingEnts[0])) {
            baseExposure = existingComp->baseAmbientExposure;
        }
    } else {
        auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
        if (!settingsEnts.empty()) {
            if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEnts[0])) {
                baseExposure = pp->ambientExposure;
            }
        }
    }
    boltComp.baseAmbientExposure = baseExposure;

    auto& bolt        = reg.Add(boltEntity, std::move(boltComp));
    bolt.cloudOrigin  = JPH::Vec3(groundPos.GetX(), static_cast<float>(cloudPos.GetY()), groundPos.GetZ());
    bolt.groundTarget = JPH::Vec3(groundPos);

    // 1. Generate fractal segments and camera-facing ribbon mesh
    std::mt19937 rng(std::random_device {}());
    bolt.segments = LightningGenerator::GenerateFractalSegments(bolt.cloudOrigin, bolt.groundTarget, cfg.ribbonWidth, cfg, rng);
    auto ribbon   = LightningGenerator::BuildCameraFacingRibbon(bolt.segments, engine.GetCamera().position);

    // 2. Allocate VBOs
    bolt.vboPos      = rc.CreateVertexBuffer(ribbon.positions.data(), ribbon.positions.size() * sizeof(VertexPosition));
    bolt.vboAttr     = rc.CreateVertexBuffer(ribbon.attributes.data(), ribbon.attributes.size() * sizeof(VertexAttributes));
    bolt.maxVertices = ribbon.maxVertices;

    // 3. Register GPU Assets
    bolt.meshAssetId = HashAssetID("lightning_mesh_" + std::to_string(boltEntity.index));
    bolt.matAssetId  = HashAssetID("lightning_mat_" + std::to_string(boltEntity.index));

    rc.RegisterGPUMesh(bolt.meshAssetId, Mesh {.posBuffer = bolt.vboPos, .attrBuffer = bolt.vboAttr, .vertexCount = 0});

    auto matRes = CreativeWorksFactory::CreateBasicMaterial(rc, true, true);
    if (matRes) {
        Material mat           = matRes.value();
        mat.baseColorFactor[0] = 1.0f;
        mat.baseColorFactor[1] = 1.0f;
        mat.baseColorFactor[2] = 1.0f;
        mat.baseColorFactor[3] = 1.0f;
        rc.RegisterGPUMaterial(bolt.matAssetId, mat);
    }

    // 4. Attach Mesh Component
    reg.Add(boltEntity, Components::TransformComponent {});
    reg.Add(boltEntity, Components::NameComponent {.name = String64("ProceduralLightning")});
    reg.Add(
        boltEntity,
        Components::MeshComponent {.meshAsset = bolt.meshAssetId, .materialAsset = bolt.matAssetId, .cullRadius = 20000.0f, .flags = DrawFlags::ExcludeFromTLAS}
    );

    // 5. Spawn point lights for local flash & impact point
    JPH::Vec3 flashPos    = engine.GetCamera().position + JPH::Vec3(0.0f, 25.0f, 0.0f);
    bolt.flashLightEntity = reg.Create();
    reg.Add(bolt.flashLightEntity, Components::TransformComponent {.position = flashPos});
    reg.Add(
        bolt.flashLightEntity, Components::LightComponent {
                                   .type        = LightType::Point,
                                   .color       = JPH::Vec3(0.88f, 0.95f, 1.0f),
                                   .intensity   = 0.0f,
                                   .radius      = 12.0f,
                                   .direction   = JPH::Vec3(0.0f, -1.0f, 0.0f),
                                   .range       = 2000.0f,
                                   .points      = JPH::Mat44::sIdentity(),
                                   .twoSided    = 0,
                                   .shadowLayer = -1
                               }
    );

    JPH::Vec3 impactPos    = bolt.groundTarget + JPH::Vec3(0.0f, 20.0f, 0.0f);
    bolt.impactLightEntity = reg.Create();
    reg.Add(bolt.impactLightEntity, Components::TransformComponent {.position = impactPos});
    reg.Add(
        bolt.impactLightEntity, Components::LightComponent {
                                    .type        = LightType::Point,
                                    .color       = JPH::Vec3(1.0f, 1.0f, 1.0f),
                                    .intensity   = 0.0f,
                                    .radius      = 8.0f,
                                    .direction   = JPH::Vec3(0.0f, 1.0f, 0.0f),
                                    .range       = 500.0f,
                                    .points      = JPH::Mat44::sIdentity(),
                                    .twoSided    = 0,
                                    .shadowLayer = -1
                                }
    );

    bolt.phase = LightningPhase::SteppedLeader;
    return boltEntity;
}

/**
 * @brief System update: advances strikes, evaluates Heidler curves, drives GPU material glow, and updates lights.
 */
void Update(Engine& engine, float dt) {
    auto& rc   = engine.GetRenderContext();
    auto& reg  = engine.GetRegistry();
    auto  ents = reg.GetEntitiesWith<LightningComponent>();

    if (ents.empty()) {
        return;
    }

    auto bolts = reg.GetRawArray<LightningComponent>();

    std::vector<Entity> deadEntities;
    float               peakLuminanceThisFrame = 0.0f;
    float               unflashedBaseExposure  = 4.5f;
    bool                hasActiveBolts         = false;

    for (size_t i = 0; i < ents.size(); ++i) {
        Entity              e    = ents[i];
        LightningComponent& bolt = bolts[i];

        if (bolt.phase == LightningPhase::Idle) {
            continue;
        }

        hasActiveBolts        = true;
        unflashedBaseExposure = bolt.baseAmbientExposure;

        float dtReal = dt / bolt.config.timeDilation;
        bolt.realTime += dtReal;
        bolt.phaseTime += dtReal;

        switch (bolt.phase) {
            case LightningPhase::SteppedLeader: {
                float growthRate = static_cast<float>(bolt.maxVertices) / 0.04f;
                bolt.visibleVertices += static_cast<uint32_t>(growthRate * dtReal);

                if (bolt.visibleVertices >= bolt.maxVertices) {
                    bolt.visibleVertices = bolt.maxVertices;
                    bolt.phase           = LightningPhase::ReturnStroke;
                    bolt.phaseTime       = 0.0f;

                    // Play 3D thunder
                    engine.GetAudioContext().PlayOneShot3D("resources/assets/audio/lightning.wav", bolt.groundTarget, bolt.config.soundVolume);
                }
                break;
            }

            case LightningPhase::ReturnStroke: {
                float tUs  = bolt.phaseTime * 1.0e6f;
                float iNow = LightningGenerator::EvaluateHeidler(tUs, bolt.config.peakCurrentKA, 1.8f, 95.0f);

                bolt.currentKA      = iNow;
                bolt.flashLuminance = std::pow(std::max(iNow, 0.0f) / 30.0f, 1.4f);

                if (bolt.phaseTime > 0.15f) {
                    bolt.phase     = LightningPhase::Dissipating;
                    bolt.phaseTime = 0.0f;
                }
                break;
            }

            case LightningPhase::Dissipating: {
                float fade          = std::exp(-bolt.phaseTime * 8.0f);
                bolt.flashLuminance = fade * 0.2f;

                if (fade < 0.01f) {
                    bolt.phase = LightningPhase::Idle;
                    deadEntities.push_back(e);
                    continue; // Skip rendering on final frame
                }
                break;
            }
            default:
                break;
        }

        peakLuminanceThisFrame = std::max(peakLuminanceThisFrame, bolt.flashLuminance);

        // --- GPU MATERIAL GLOW & LIGHTING UPDATES ---
        if (auto gpuMatOpt = rc.GetGPUMaterial(bolt.matAssetId)) {
            Material mat          = *gpuMatOpt;
            float    intensity    = (bolt.phase == LightningPhase::SteppedLeader) ? 15.0f : (bolt.flashLuminance * bolt.config.emissiveIntensity);
            mat.emissiveFactor[0] = 0.88f * intensity;
            mat.emissiveFactor[1] = 0.95f * intensity;
            mat.emissiveFactor[2] = 1.00f * intensity;
            rc.RegisterGPUMaterial(bolt.matAssetId, mat);
        }

        if (auto gpuMeshOpt = rc.GetGPUMesh(bolt.meshAssetId)) {
            Mesh m        = *gpuMeshOpt;
            m.vertexCount = bolt.visibleVertices;
            rc.RegisterGPUMesh(bolt.meshAssetId, m);
        }

        if (reg.IsAlive(bolt.flashLightEntity)) {
            Patch<Components::LightComponent>(reg, bolt.flashLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 8000000.0f; });
        }
        if (reg.IsAlive(bolt.impactLightEntity)) {
            Patch<Components::LightComponent>(reg, bolt.impactLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 4000000.0f; });
        }
    }

    // Apply combined peak flash luminance to global post-process settings
    auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (hasActiveBolts && !settingsEnts.empty()) {
        Patch<Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](auto& pp) {
            pp.ambientExposure = unflashedBaseExposure + (180.0f * peakLuminanceThisFrame);
        });
    }

    // Clean up expired entities
    for (Entity deadEnt: deadEntities) {
        if (auto* bolt = reg.Get<LightningComponent>(deadEnt)) {
            if (bolt->flashLightEntity != NullEntity && reg.IsAlive(bolt->flashLightEntity)) {
                reg.Destroy(bolt->flashLightEntity);
            }
            if (bolt->impactLightEntity != NullEntity && reg.IsAlive(bolt->impactLightEntity)) {
                reg.Destroy(bolt->impactLightEntity);
            }
        }
        reg.Destroy(deadEnt);
    }

    // --- FIX FOR AMBIENCE STACKING BUG ---
    // Restore ambient exposure to the un-flashed baseline when all active bolts expire
    if (reg.GetEntitiesWith<LightningComponent>().empty() && !settingsEnts.empty()) {
        Patch<Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](auto& pp) { pp.ambientExposure = unflashedBaseExposure; });
    }
}

} // namespace ZHLN::Lightning

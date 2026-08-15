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
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Print.hpp>
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
#include <string_view>
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
// 2. ECS COMPONENT (Pure DOD Data Storage - Zero Dead Memory)
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

inline auto GenerateFractalSegments(JPH::Vec3Arg start, JPH::Vec3Arg end, float startWidth, const LightningConfig& config, std::mt19937& rng)
    -> std::vector<LightningSegment> {
    std::vector<LightningSegment> queue;
    queue.push_back({.start = start, .end = end, .width = startWidth, .branchLevel = 0.0f});

    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> prob(0.0f, 1.0f);

    for (int step = 0; step < config.subdivisions; ++step) {
        std::vector<LightningSegment> nextQueue;
        const float                   scale = std::pow(0.55f, static_cast<float>(step)) * (end - start).Length() * 0.18f;

        for (const auto& seg: queue) {
            const JPH::Vec3 mid = (seg.start + seg.end) * 0.5f;
            const JPH::Vec3 dir = (seg.end - seg.start);
            const float     len = dir.Length();

            if (len < 0.1f) {
                nextQueue.push_back(seg);
                continue;
            }
            const JPH::Vec3 dirNorm = dir / len;

            JPH::Vec3 side     = dirNorm.Cross(JPH::Vec3::sAxisY());
            side               = (side.LengthSq() < 1e-4f) ? JPH::Vec3::sAxisX() : side.Normalized();
            const JPH::Vec3 up = dirNorm.Cross(side).Normalized();

            const JPH::Vec3 offset      = (side * dist(rng) + up * dist(rng)) * scale;
            const JPH::Vec3 jitteredMid = mid + offset;

            nextQueue.push_back({.start = seg.start, .end = jitteredMid, .width = seg.width, .branchLevel = seg.branchLevel});
            nextQueue.push_back({.start = jitteredMid, .end = seg.end, .width = seg.width, .branchLevel = seg.branchLevel});

            const float branchProb = (0.35f * config.eta / 2.0f) / (1.0f + seg.branchLevel * 0.8f);
            if (prob(rng) < branchProb) {
                const JPH::Vec3 branchDir = (dirNorm + (side * dist(rng) + up * dist(rng)) * 0.7f).Normalized();
                const float     branchLen = len * (0.4f + prob(rng) * 0.3f);
                const JPH::Vec3 branchEnd = jitteredMid + branchDir * branchLen;

                nextQueue.push_back({.start = jitteredMid, .end = branchEnd, .width = seg.width * 0.5f, .branchLevel = seg.branchLevel + 1.0f});
            }
        }
        queue = std::move(nextQueue);
    }
    return queue;
}

inline auto BuildCameraFacingRibbon(std::span<const LightningSegment> segments, JPH::Vec3Arg cameraPos) -> GeneratedRibbon {
    GeneratedRibbon ribbon;
    ribbon.positions.reserve(segments.size() * 6);
    ribbon.attributes.reserve(segments.size() * 6);

    const auto n    = Math::PackNormal(0, 1, 0);
    const auto t    = Math::PackNormal(1, 0, 0, 1);
    const auto c    = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);
    const auto uv00 = Math::PackUV(0.0f, 0.0f);
    const auto uv10 = Math::PackUV(1.0f, 0.0f);
    const auto uv01 = Math::PackUV(0.0f, 1.0f);
    const auto uv11 = Math::PackUV(1.0f, 1.0f);

    for (const auto& seg: segments) {
        const JPH::Vec3 p0 = seg.start;
        const JPH::Vec3 p1 = seg.end;

        const JPH::Vec3 dir = (p1 - p0);
        const float     len = dir.Length();
        if (len < 1e-4f) {
            continue;
        }
        const JPH::Vec3 dirNorm = dir / len;

        const JPH::Vec3 toCamDiff = cameraPos - p0;
        const JPH::Vec3 toCam     = (toCamDiff.LengthSq() < 1e-4f) ? JPH::Vec3::sAxisZ() : toCamDiff.Normalized();

        JPH::Vec3 side = dirNorm.Cross(toCam);
        side           = (side.LengthSq() < 1e-4f) ? JPH::Vec3::sAxisX() : side.Normalized();

        const float     width = seg.width;
        const JPH::Vec3 v0    = p0 - side * width;
        const JPH::Vec3 v1    = p0 + side * width;
        const JPH::Vec3 v2    = p1 - side * width;
        const JPH::Vec3 v3    = p1 + side * width;

        ribbon.positions.push_back({{v0.GetX(), v0.GetY(), v0.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv00, .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv10, .color = c});
        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv01, .color = c});

        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv01, .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv10, .color = c});
        ribbon.positions.push_back({{v3.GetX(), v3.GetY(), v3.GetZ()}});
        ribbon.attributes.push_back({.normal = n, .tangent = t, .uv = uv11, .color = c});
    }

    ribbon.maxVertices = static_cast<uint32_t>(ribbon.positions.size());
    return ribbon;
}

[[nodiscard]] inline auto EvaluateHeidler(float tUs, float i0, float t1, float t2) noexcept -> float {
    if (tUs <= 0.0f || i0 <= 0.0f) {
        return 0.0f;
    }
    const float x  = (tUs / t1) * (tUs / t1);
    const float ec = std::exp(-(t1 / t2) * std::sqrt(2.0f * t2 / t1));
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
auto Spawn(Engine& engine, JPH::RVec3Arg cloudPos, JPH::RVec3Arg groundPos, const LightningConfig& cfg = {}) -> Entity {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    // Cache un-flashed baseline exposure
    float      baseExposure = 4.5f;
    const auto existingEnts = reg.GetEntitiesWith<LightningComponent>();
    if (!existingEnts.empty()) {
        if (const auto* existingComp = reg.Get<LightningComponent>(existingEnts[0])) {
            baseExposure = existingComp->baseAmbientExposure;
        }
    } else {
        const auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
        if (!settingsEnts.empty()) {
            if (const auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEnts[0])) {
                baseExposure = pp->ambientExposure;
            }
        }
    }

    // Thread-local PRNG eliminates kernel entropy syscalls
    static thread_local std::mt19937 s_rng(std::random_device {}());

    const JPH::Vec3 cloudOrigin(groundPos.GetX(), static_cast<float>(cloudPos.GetY()), groundPos.GetZ());
    const JPH::Vec3 groundTarget(groundPos);

    // 1. Generate fractal segments & ribbon
    const auto segments = LightningGenerator::GenerateFractalSegments(cloudOrigin, groundTarget, cfg.ribbonWidth, cfg, s_rng);
    const auto ribbon   = LightningGenerator::BuildCameraFacingRibbon(segments, engine.GetCamera().position);

    // 2. Allocate VBOs
    const BufferHandle vboPos  = rc.CreateVertexBuffer(ribbon.positions.data(), ribbon.positions.size() * sizeof(VertexPosition));
    const BufferHandle vboAttr = rc.CreateVertexBuffer(ribbon.attributes.data(), ribbon.attributes.size() * sizeof(VertexAttributes));

    // 3. Create Entity & Register GPU Assets using zero-allocation stack formatting
    const Entity boltEntity = reg.Create();

    std::array<char, 64> strBuf {};
    const AssetID        meshAssetId = HashAssetID(FormatTo(strBuf, "lightning_mesh_{}", boltEntity.index));
    const MaterialID     matAssetId  = HashAssetID(FormatTo(strBuf, "lightning_mat_{}", boltEntity.index));

    rc.RegisterGPUMesh(meshAssetId, Mesh {.posBuffer = vboPos, .attrBuffer = vboAttr, .vertexCount = 0});

    if (auto matRes = CreativeWorksFactory::CreateMaterial(
            rc, {.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}}
        )) {
        rc.RegisterGPUMaterial(matAssetId, *matRes);
    }

    // 4. Point lights for local flash and ground strike impact
    const JPH::Vec3 flashPos  = engine.GetCamera().position + JPH::Vec3(0.0f, 25.0f, 0.0f);
    const JPH::Vec3 impactPos = groundTarget + JPH::Vec3(0.0f, 20.0f, 0.0f);

    const Entity flashLight = reg.Create(
        Components::TransformComponent {.position = flashPos}, Components::LightComponent {
                                                                   .type        = LightType::Point,
                                                                   .color       = JPH::Vec3(0.88f, 0.95f, 1.0f),
                                                                   .intensity   = 0.0f,
                                                                   .radius      = 12.0f,
                                                                   .direction   = JPH::Vec3(0.0f, -1.0f, 0.0f),
                                                                   .range       = 2000.0f,
                                                                   .shadowLayer = -1
                                                               }
    );

    const Entity impactLight = reg.Create(
        Components::TransformComponent {.position = impactPos}, Components::LightComponent {
                                                                    .type        = LightType::Point,
                                                                    .color       = JPH::Vec3(1.0f, 1.0f, 1.0f),
                                                                    .intensity   = 0.0f,
                                                                    .radius      = 8.0f,
                                                                    .direction   = JPH::Vec3(0.0f, 1.0f, 0.0f),
                                                                    .range       = 500.0f,
                                                                    .shadowLayer = -1
                                                                }
    );

    // 5. Attach root bolt components atomically
    reg.Add(
        boltEntity, Components::TransformComponent {}, Components::NameComponent {.name = String64("ProceduralLightning")},
        Components::MeshComponent {.meshAsset = meshAssetId, .materialAsset = matAssetId, .cullRadius = 20000.0f, .flags = DrawFlags::ExcludeFromTLAS},
        LightningComponent {
            .config              = cfg,
            .phase               = LightningPhase::SteppedLeader,
            .cloudOrigin         = cloudOrigin,
            .groundTarget        = groundTarget,
            .vboPos              = vboPos,
            .vboAttr             = vboAttr,
            .meshAssetId         = meshAssetId,
            .matAssetId          = matAssetId,
            .maxVertices         = ribbon.maxVertices,
            .visibleVertices     = 0,
            .flashLightEntity    = flashLight,
            .impactLightEntity   = impactLight,
            .baseAmbientExposure = baseExposure
        }
    );

    return boltEntity;
}

/**
 * @brief System update: advances strikes, evaluates Heidler curves, drives GPU material glow, and updates lights.
 */
auto Update(Engine& engine, float dt) -> void {
    auto&      rc   = engine.GetRenderContext();
    auto&      reg  = engine.GetRegistry();
    const auto ents = reg.GetEntitiesWith<LightningComponent>();

    if (ents.empty()) {
        return;
    }

    auto bolts = reg.GetRawArray<LightningComponent>();

    std::vector<Entity> deadEntities;
    float               peakLuminanceThisFrame = 0.0f;
    float               unflashedBaseExposure  = 4.5f;
    bool                hasActiveBolts         = false;

    for (size_t i = 0; i < ents.size(); ++i) {
        const Entity        e    = ents[i];
        LightningComponent& bolt = bolts[i];

        if (bolt.phase == LightningPhase::Idle) {
            continue;
        }

        hasActiveBolts        = true;
        unflashedBaseExposure = bolt.baseAmbientExposure;

        const float dtReal = dt / bolt.config.timeDilation;
        bolt.realTime += dtReal;
        bolt.phaseTime += dtReal;

        switch (bolt.phase) {
            case LightningPhase::SteppedLeader: {
                const float growthRate = static_cast<float>(bolt.maxVertices) / 0.04f;
                bolt.visibleVertices += static_cast<uint32_t>(growthRate * dtReal);

                if (bolt.visibleVertices >= bolt.maxVertices) {
                    bolt.visibleVertices = bolt.maxVertices;
                    bolt.phase           = LightningPhase::ReturnStroke;
                    bolt.phaseTime       = 0.0f;

                    engine.GetAudioContext().PlayOneShot3D("resources/assets/audio/lightning.wav", bolt.groundTarget, bolt.config.soundVolume);
                }
                break;
            }

            case LightningPhase::ReturnStroke: {
                const float tNorm = std::clamp(bolt.phaseTime / 0.15f, 0.0f, 1.0f);
                const float tUs   = tNorm * 200.0f;
                const float iNow  = LightningGenerator::EvaluateHeidler(tUs, bolt.config.peakCurrentKA, 1.8f, 95.0f);

                bolt.currentKA      = iNow;
                bolt.flashLuminance = std::pow(std::max(iNow, 0.0f) / 30.0f, 1.4f);

                if (bolt.phaseTime > 0.15f) {
                    bolt.phase     = LightningPhase::Dissipating;
                    bolt.phaseTime = 0.0f;
                }
                break;
            }

            case LightningPhase::Dissipating: {
                const float fade    = std::exp(-bolt.phaseTime * 8.0f);
                bolt.flashLuminance = fade * 0.2f;

                if (fade < 0.01f) {
                    bolt.phase = LightningPhase::Idle;
                    deadEntities.push_back(e);
                    continue;
                }
                break;
            }
            default:
                break;
        }

        peakLuminanceThisFrame = std::max(peakLuminanceThisFrame, bolt.flashLuminance);

        // Update GPU material emissive glow & mesh vertex count
        if (auto gpuMatOpt = rc.GetGPUMaterial(bolt.matAssetId)) {
            Material    mat       = *gpuMatOpt;
            const float intensity = (bolt.phase == LightningPhase::SteppedLeader) ? 15.0f : (bolt.flashLuminance * bolt.config.emissiveIntensity);

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
            ECS::Patch<Components::LightComponent>(reg, bolt.flashLightEntity, [&](auto& light) -> auto {
                light.intensity = bolt.flashLuminance * 8000000.0f;
            });
        }
        if (reg.IsAlive(bolt.impactLightEntity)) {
            ECS::Patch<Components::LightComponent>(reg, bolt.impactLightEntity, [&](auto& light) -> auto {
                light.intensity = bolt.flashLuminance * 4000000.0f;
            });
        }
    }

    // Apply combined peak flash luminance to global post-process settings
    const auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (hasActiveBolts && !settingsEnts.empty()) {
        ECS::Patch<Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](auto& pp) -> auto {
            pp.ambientExposure = unflashedBaseExposure + (180.0f * peakLuminanceThisFrame);
        });
    }

    // Clean up expired entities
    for (const Entity deadEnt: deadEntities) {
        if (const auto* bolt = reg.Get<LightningComponent>(deadEnt)) {
            if (bolt->flashLightEntity != NullEntity && reg.IsAlive(bolt->flashLightEntity)) {
                reg.Destroy(bolt->flashLightEntity);
            }
            if (bolt->impactLightEntity != NullEntity && reg.IsAlive(bolt->impactLightEntity)) {
                reg.Destroy(bolt->impactLightEntity);
            }
        }
        reg.Destroy(deadEnt);
    }

    // Restore ambient exposure to baseline when all active bolts expire
    if (reg.GetEntitiesWith<LightningComponent>().empty() && !settingsEnts.empty()) {
        ECS::Patch<Components::PostProcessSettingsComponent>(reg, settingsEnts[0], [&](auto& pp) -> auto { pp.ambientExposure = unflashedBaseExposure; });
    }
}

} // namespace ZHLN::Lightning

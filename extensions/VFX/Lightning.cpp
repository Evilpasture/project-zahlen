// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#if defined(_WIN32)
// MinGW's windows.h declares x86 intrinsics; load it before Jolt's immintrin.h.
#include <Zahlen/Core/Platform.hpp>
#endif

// --- Global Module Fragment: Implementation Dependencies ---
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Module Implementation Unit declaration (DO NOT use "import ZHLN.Lightning;")
module ZHLN.Lightning;

namespace ZHLN {

namespace {

struct LightningSegment {
    JPH::Vec3 start;
    JPH::Vec3 end;
    float     width;
    float     branchLevel;
};

struct GeneratedRibbon {
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
    uint32_t                        maxVertices = 0;
};

auto GenerateFractalSegments(JPH::Vec3Arg start, JPH::Vec3Arg end, float startWidth, const LightningConfig& config, std::mt19937& rng)
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

auto BuildCameraFacingRibbon(std::span<const LightningSegment> segments, JPH::Vec3Arg cameraPos) -> GeneratedRibbon {
    GeneratedRibbon ribbon;
    ribbon.positions.reserve(segments.size() * 6);
    ribbon.frames.reserve(segments.size() * 6);
    ribbon.surfaces.reserve(segments.size() * 6);

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
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv00, .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});

        ribbon.positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});
        ribbon.positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v3.GetX(), v3.GetY(), v3.GetZ()}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv11, .color = c});
    }

    ribbon.maxVertices = static_cast<uint32_t>(ribbon.positions.size());
    return ribbon;
}

auto EvaluateHeidler(float tUs, float i0, float t1, float t2) noexcept -> float {
    if (tUs <= 0.0f || i0 <= 0.0f) {
        return 0.0f;
    }
    const float x  = (tUs / t1) * (tUs / t1);
    const float ec = std::exp(-(t1 / t2) * std::sqrt(2.0f * t2 / t1));
    return (i0 / ec) * (x / (1.0f + x)) * std::exp(-tUs / t2);
}

void ReleaseLightning(Engine& engine, LightningComponent& bolt) {
    auto& render = engine.GetRenderContext();
    if (bolt.meshAssetId != InvalidAssetID) {
        render.UnregisterGPUMesh(bolt.meshAssetId);
    }
    if (bolt.matAssetId != InvalidMaterialID) {
        render.UnregisterGPUMaterial(bolt.matAssetId);
    }
    render.DestroyMesh(
        Mesh {
            .posBuffer          = std::exchange(bolt.vboPos, BufferHandle::Invalid),
            .tangentFrameBuffer = std::exchange(bolt.vboFrame, BufferHandle::Invalid),
            .surfaceBuffer      = std::exchange(bolt.vboSurface, BufferHandle::Invalid)
        }
    );
    bolt.meshAssetId = InvalidAssetID;
    bolt.matAssetId  = InvalidMaterialID;
}

void CleanupLightning(Engine& engine, bool all) {
    auto&      reg      = engine.GetRegistry();
    const auto entities = reg.GetEntitiesWith<LightningComponent>();
    if (entities.empty()) {
        return;
    }
    auto bolts = reg.GetRawArray<LightningComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (all || reg.Get<Components::PendingDestroy>(entities[i])) {
            ReleaseLightning(engine, bolts[i]);
        }
    }
}

void RegisterCleanup(Engine& engine) {
    auto& reg = engine.GetRegistry();
    reg.RegisterComponent<LightningComponent>("LightningComponent");
    if (engine.AddSceneCleanupPass(&CleanupLightning)) {
        engine.AddDeviceLostCallback([](Engine& owner) {
            for (auto& bolt: owner.GetRegistry().GetRawArray<LightningComponent>()) {
                bolt.vboPos     = BufferHandle::Invalid;
                bolt.vboFrame   = BufferHandle::Invalid;
                bolt.vboSurface = BufferHandle::Invalid;
            }
        });
    }
}

} // namespace

namespace Lightning {

void Detach(Engine& engine, Entity entity) {
    if (auto bolt = engine.GetRegistry().Get<LightningComponent>(entity)) {
        ReleaseLightning(engine, *bolt);
        engine.GetRegistry().Remove<LightningComponent>(entity);
    }
}

void Attach(Engine& engine, Entity entity, LightningComponent component) {
    RegisterCleanup(engine);
    Detach(engine, entity);
    engine.GetRegistry().Add(entity, std::move(component));
}

auto Spawn(Engine& engine, JPH::RVec3Arg cloudPos, JPH::RVec3Arg groundPos, const LightningConfig& cfg) -> Entity {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    RegisterCleanup(engine);

    float      baseExposure = 4.5f;
    const auto existingEnts = reg.GetEntitiesWith<LightningComponent>();
    if (!existingEnts.empty()) {
        if (const auto existingComp = reg.Get<LightningComponent>(existingEnts[0])) {
            baseExposure = existingComp->baseAmbientExposure;
        }
    } else {
        const auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
        if (!settingsEnts.empty()) {
            if (const auto pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEnts[0])) {
                baseExposure = pp->ambientExposure;
            }
        }
    }

    static thread_local std::mt19937 s_rng(std::random_device {}());

    const JPH::Vec3 cloudOrigin(groundPos.GetX(), static_cast<float>(cloudPos.GetY()), groundPos.GetZ());
    const JPH::Vec3 groundTarget(groundPos);

    // The ribbon faces the view, and the flash is placed relative to it: a world
    // with no main camera has no view to face and nothing to flash, so there is no
    // bolt to spawn. The null entity says that -- the alternative was fabricating a
    // camera at the engine's default framing, then aiming the ribbon and the flash
    // at a point no observer occupies.
    const auto camComp = reg.GetSingleton<Components::CameraComponent>();
    if (!camComp) {
        return Entity::Null();
    }

    const auto segments = GenerateFractalSegments(cloudOrigin, groundTarget, cfg.ribbonWidth, cfg, s_rng);
    const JPH::Vec3 cameraPosition = camComp->camera.position;
    const auto      ribbon         = BuildCameraFacingRibbon(segments, cameraPosition);

    const BufferHandle vboPos     = rc.CreateVertexBuffer(std::span {ribbon.positions});
    const BufferHandle vboFrame   = rc.CreateVertexBuffer(std::span {ribbon.frames});
    const BufferHandle vboSurface = rc.CreateVertexBuffer(std::span {ribbon.surfaces});

    const Entity boltEntity = reg.Create();

    std::array<char, 64> strBuf {};
    const AssetID        meshAssetId = HashAssetID(FormatTo(strBuf, "lightning_mesh_{}", boltEntity.index));
    const MaterialID     matAssetId  = HashAssetID(FormatTo(strBuf, "lightning_mat_{}", boltEntity.index));

    rc.RegisterGPUMesh(meshAssetId, Mesh {.posBuffer = vboPos, .tangentFrameBuffer = vboFrame, .surfaceBuffer = vboSurface, .vertexCount = 0});

    if (auto matRes =
            rc.CreateMaterial({.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}})) {
        rc.RegisterGPUMaterial(matAssetId, *matRes);
    }

    const JPH::Vec3 flashPos  = cameraPosition + JPH::Vec3(0.0f, 25.0f, 0.0f);
    const JPH::Vec3 impactPos = groundTarget + JPH::Vec3(0.0f, 20.0f, 0.0f);

    const Entity flashLight = reg.Create(
        Components::TransformComponent {.position = flashPos}, Components::HierarchyComponent {.parent = boltEntity},
        Components::LightComponent {
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
        Components::TransformComponent {.position = impactPos}, Components::HierarchyComponent {.parent = boltEntity},
        Components::LightComponent {
            .type        = LightType::Point,
            .color       = JPH::Vec3(1.0f, 1.0f, 1.0f),
            .intensity   = 0.0f,
            .radius      = 8.0f,
            .direction   = JPH::Vec3(0.0f, 1.0f, 0.0f),
            .range       = 500.0f,
            .shadowLayer = -1
        }
    );

    reg.Add(
        boltEntity, Components::TransformComponent {}, Components::NameComponent {.name = String64("ProceduralLightning")},
        Components::MeshComponent {.meshAsset = meshAssetId, .materialAsset = matAssetId, .cullRadius = 20000.0f, .flags = DrawFlags::ExcludeFromTLAS},
        LightningComponent {
            .config              = cfg,
            .phase               = LightningPhase::SteppedLeader,
            .cloudOrigin         = cloudOrigin,
            .groundTarget        = groundTarget,
            .vboPos              = vboPos,
            .vboFrame            = vboFrame,
            .vboSurface          = vboSurface,
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

        if (bolt.phase == LightningPhase::Idle || reg.Get<Components::PendingDestroy>(e)) {
            continue;
        }

        // A rebuilt renderer has no cached mesh or material. Restore them from
        // component data, never from an entity-keyed buffer table.
        bool needsMeshRegistration = !rc.GetGPUMesh(bolt.meshAssetId).has_value();
        if (bolt.vboPos == BufferHandle::Invalid || bolt.vboFrame == BufferHandle::Invalid || bolt.vboSurface == BufferHandle::Invalid) {
            const auto camComp = reg.GetSingleton<Components::CameraComponent>();
            if (!camComp) {
                // No view to face: the ribbon cannot be rebuilt, and the handles stay
                // invalid so the first frame that has a camera retries them. Building
                // it against a fabricated pose would bake the wrong geometry in.
                continue;
            }
            static thread_local std::mt19937 rng(std::random_device {}());
            const auto                       segments = GenerateFractalSegments(bolt.cloudOrigin, bolt.groundTarget, bolt.config.ribbonWidth, bolt.config, rng);
            const JPH::Vec3                  cameraPosition = camComp->camera.position;
            const auto                       ribbon   = BuildCameraFacingRibbon(segments, cameraPosition);
            if (bolt.vboPos == BufferHandle::Invalid) {
                bolt.vboPos = rc.CreateVertexBuffer(std::span {ribbon.positions});
            }
            if (bolt.vboFrame == BufferHandle::Invalid) {
                bolt.vboFrame = rc.CreateVertexBuffer(std::span {ribbon.frames});
            }
            if (bolt.vboSurface == BufferHandle::Invalid) {
                bolt.vboSurface = rc.CreateVertexBuffer(std::span {ribbon.surfaces});
            }
            bolt.maxVertices      = ribbon.maxVertices;
            bolt.visibleVertices  = std::min(bolt.visibleVertices, bolt.maxVertices);
            needsMeshRegistration = true;
        }
        if (needsMeshRegistration && bolt.vboPos != BufferHandle::Invalid && bolt.vboFrame != BufferHandle::Invalid &&
            bolt.vboSurface != BufferHandle::Invalid) {
            rc.RegisterGPUMesh(
                bolt.meshAssetId,
                Mesh {.posBuffer = bolt.vboPos, .tangentFrameBuffer = bolt.vboFrame, .surfaceBuffer = bolt.vboSurface, .vertexCount = bolt.visibleVertices}
            );
        }
        if (!rc.GetGPUMaterial(bolt.matAssetId)) {
            if (auto mat = rc.CreateMaterial(
                    {.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}}
                )) {
                rc.RegisterGPUMaterial(bolt.matAssetId, *mat);
            }
        }

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

                    engine.GetAudioContext().PostEvent(
                        {.type     = AudioEventType::OneShot3D,
                         .filepath = "resources/assets/audio/lightning.wav",
                         .position = bolt.groundTarget,
                         .volume   = bolt.config.soundVolume}
                    );
                }
                break;
            }

            case LightningPhase::ReturnStroke: {
                const float tNorm = std::clamp(bolt.phaseTime / 0.15f, 0.0f, 1.0f);
                const float tUs   = tNorm * 200.0f;
                const float iNow  = EvaluateHeidler(tUs, bolt.config.peakCurrentKA, 1.8f, 95.0f);

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

        hasActiveBolts         = true;
        peakLuminanceThisFrame = std::max(peakLuminanceThisFrame, bolt.flashLuminance);

        if (auto gpuMatOpt = rc.GetGPUMaterial(bolt.matAssetId)) {
            Material    mat       = *gpuMatOpt;
            const float intensity = (bolt.phase == LightningPhase::SteppedLeader) ? 15.0f : (bolt.flashLuminance * bolt.config.emissiveIntensity);

            // Lanes 0-2 only: the fourth lane belongs to the ABI's emissive
            // packing and this path never owned it.
            mat.emissiveFactor.x  = 0.88f * intensity;
            mat.emissiveFactor.y  = 0.95f * intensity;
            mat.emissiveFactor.z  = 1.00f * intensity;
            rc.RegisterGPUMaterial(bolt.matAssetId, mat);
        }

        if (auto gpuMeshOpt = rc.GetGPUMesh(bolt.meshAssetId)) {
            Mesh m        = *gpuMeshOpt;
            m.vertexCount = bolt.visibleVertices;
            rc.RegisterGPUMesh(bolt.meshAssetId, m);
        }

        if (reg.IsAlive(bolt.flashLightEntity)) {
            reg.Patch<Components::LightComponent>(bolt.flashLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 8000000.0f; });
        }
        if (reg.IsAlive(bolt.impactLightEntity)) {
            reg.Patch<Components::LightComponent>(bolt.impactLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 4000000.0f; });
        }
    }

    const auto settingsEnts = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (hasActiveBolts && !settingsEnts.empty()) {
        reg.Patch<Components::PostProcessSettingsComponent>(settingsEnts[0], [&](auto& pp) {
            pp.ambientExposure = unflashedBaseExposure + (180.0f * peakLuminanceThisFrame);
        });
    }

    for (const Entity deadEnt: deadEntities) {
        // The scene cleanup pass releases VBOs and child lights after the
        // simulation step, while the LightningComponent is still inspectable.
        DespawnEntity(engine, deadEnt);
    }

    if (!hasActiveBolts && !settingsEnts.empty()) {
        reg.Patch<Components::PostProcessSettingsComponent>(settingsEnts[0], [&](auto& pp) { pp.ambientExposure = unflashedBaseExposure; });
    }
}

} // namespace Lightning

} // namespace ZHLN

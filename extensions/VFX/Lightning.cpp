// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Math/Vec3.hpp>

// --- Global Module Fragment: Implementation Dependencies ---
// clang-format off
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Math/Vec3.hpp>
// clang-format on
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <Zahlen/ECS/ECS.hpp>
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
    FVec3 start;
    FVec3 end;
    float     width;
    float     branchLevel;
};

struct GeneratedRibbon {
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
    uint32_t                        maxVertices = 0;
};

auto GenerateFractalSegments(FVec3 start, FVec3 end, float startWidth, const LightningConfig& config, std::mt19937& rng)
    -> std::vector<LightningSegment> {
    std::vector<LightningSegment> queue;
    queue.push_back({.start = start, .end = end, .width = startWidth, .branchLevel = 0.0f});

    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> prob(0.0f, 1.0f);

    for (int step = 0; step < config.subdivisions; ++step) {
        std::vector<LightningSegment> nextQueue;
        const float                   scale = std::pow(0.55f, static_cast<float>(step)) * (end - start).Length() * 0.18f;

        for (const auto& seg: queue) {
            const FVec3 mid = (seg.start + seg.end) * 0.5f;
            const FVec3 dir = (seg.end - seg.start);
            const float     len = dir.Length();

            if (len < 0.1f) {
                nextQueue.push_back(seg);
                continue;
            }
            const FVec3 dirNorm = dir / len;

            FVec3 side     = Cross(dirNorm, FVec3::UnitY());
            side               = (side.LengthSquared() < 1e-4f) ? FVec3::UnitX() : side.Normalized();
            const FVec3 up = Cross(dirNorm, side).Normalized();

            const FVec3 offset      = (side * dist(rng) + up * dist(rng)) * scale;
            const FVec3 jitteredMid = mid + offset;

            nextQueue.push_back({.start = seg.start, .end = jitteredMid, .width = seg.width, .branchLevel = seg.branchLevel});
            nextQueue.push_back({.start = jitteredMid, .end = seg.end, .width = seg.width, .branchLevel = seg.branchLevel});

            const float branchProb = (0.35f * config.eta / 2.0f) / (1.0f + seg.branchLevel * 0.8f);
            if (prob(rng) < branchProb) {
                const FVec3 branchDir = (dirNorm + (side * dist(rng) + up * dist(rng)) * 0.7f).Normalized();
                const float     branchLen = len * (0.4f + prob(rng) * 0.3f);
                const FVec3 branchEnd = jitteredMid + branchDir * branchLen;

                nextQueue.push_back({.start = jitteredMid, .end = branchEnd, .width = seg.width * 0.5f, .branchLevel = seg.branchLevel + 1.0f});
            }
        }
        queue = std::move(nextQueue);
    }
    return queue;
}

auto BuildCameraFacingRibbon(std::span<const LightningSegment> segments, FVec3 cameraPos) -> GeneratedRibbon {
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
        const FVec3 p0 = seg.start;
        const FVec3 p1 = seg.end;

        const FVec3 dir = (p1 - p0);
        const float     len = dir.Length();
        if (len < 1e-4f) {
            continue;
        }
        const FVec3 dirNorm = dir / len;

        const FVec3 toCamDiff = cameraPos - p0;
        const FVec3 toCam     = (toCamDiff.LengthSquared() < 1e-4f) ? FVec3::UnitZ() : toCamDiff.Normalized();

        FVec3 side = Cross(dirNorm, toCam);
        side           = (side.LengthSquared() < 1e-4f) ? FVec3::UnitX() : side.Normalized();

        const float     width = seg.width;
        const FVec3 v0    = p0 - side * width;
        const FVec3 v1    = p0 + side * width;
        const FVec3 v2    = p1 - side * width;
        const FVec3 v3    = p1 + side * width;

        ribbon.positions.push_back({{v0.x, v0.y, v0.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv00, .color = c});
        ribbon.positions.push_back({{v1.x, v1.y, v1.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v2.x, v2.y, v2.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});

        ribbon.positions.push_back({{v2.x, v2.y, v2.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv01, .color = c});
        ribbon.positions.push_back({{v1.x, v1.y, v1.z}});
        ribbon.frames.push_back({.normal = n, .tangent = t});
        ribbon.surfaces.push_back({.uv = uv10, .color = c});
        ribbon.positions.push_back({{v3.x, v3.y, v3.z}});
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
    auto render = engine.Graphics();
    if (bolt.meshAssetId != InvalidAssetID) {
        render.UnregisterMesh(bolt.meshAssetId);
    }
    if (bolt.matAssetId != InvalidMaterialID) {
        render.UnregisterMaterial(bolt.matAssetId);
    }
    render.DestroyMesh(
        Mesh {
            .posBuffer          = std::exchange(bolt.vboPos, BufferID::Invalid),
            .tangentFrameBuffer = std::exchange(bolt.vboFrame, BufferID::Invalid),
            .surfaceBuffer      = std::exchange(bolt.vboSurface, BufferID::Invalid)
        }
    );
    bolt.meshAssetId = InvalidAssetID;
    bolt.matAssetId  = InvalidMaterialID;
}

void CleanupLightning(Engine& engine, bool all) {
    auto&      reg      = ZHLN::EngineAccess::RegistryOf(engine);
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
    auto& reg = ZHLN::EngineAccess::RegistryOf(engine);
    reg.RegisterComponent<LightningComponent>("LightningComponent");
    if (engine.AddSceneCleanupPass(&CleanupLightning)) {
        engine.AddDeviceLostCallback([](Engine& owner) {
            for (auto& bolt: ZHLN::EngineAccess::RegistryOf(owner).GetRawArray<LightningComponent>()) {
                bolt.vboPos     = BufferID::Invalid;
                bolt.vboFrame   = BufferID::Invalid;
                bolt.vboSurface = BufferID::Invalid;
            }
        });
    }
}

} // namespace

namespace Lightning {

void Detach(Engine& engine, Entity entity) {
    if (auto bolt = ZHLN::EngineAccess::RegistryOf(engine).Get<LightningComponent>(entity)) {
        ReleaseLightning(engine, *bolt);
        ZHLN::EngineAccess::RegistryOf(engine).Remove<LightningComponent>(entity);
    }
}

void Attach(Engine& engine, Entity entity, LightningComponent component) {
    RegisterCleanup(engine);
    Detach(engine, entity);
    ZHLN::EngineAccess::RegistryOf(engine).Add(entity, std::move(component));
}

auto Spawn(Engine& engine, ZHLN::DVec3 cloudPos, ZHLN::DVec3 groundPos, const LightningConfig& cfg) -> Entity {
    auto& reg = ZHLN::EngineAccess::RegistryOf(engine);
    auto rc = engine.Graphics();

    RegisterCleanup(engine);

    float      baseExposure = 4.5f;
    const auto existingEnts = reg.GetEntitiesWith<LightningComponent>();
    if (!existingEnts.empty()) {
        if (const auto existingComp = reg.Get<LightningComponent>(existingEnts[0])) {
            baseExposure = existingComp->baseAmbientExposure;
        }
    } else {
        baseExposure = engine.Graphics().Settings().environment.ambientExposure;
    }

    static thread_local std::mt19937 s_rng(std::random_device {}());

    const FVec3 cloudOrigin{static_cast<float>(groundPos.x), static_cast<float>(cloudPos.y), static_cast<float>(groundPos.z)};
    const FVec3 groundTarget {static_cast<float>(groundPos.x), static_cast<float>(groundPos.y), static_cast<float>(groundPos.z)};

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
    const FVec3 cameraPosition = camComp->camera.position;
    const auto      ribbon         = BuildCameraFacingRibbon(segments, cameraPosition);

    const BufferID vboPos     = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.positions});
    const BufferID vboFrame   = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.frames});
    const BufferID vboSurface = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.surfaces});

    const Entity boltEntity = reg.Create();

    std::array<char, 64> strBuf {};
    const AssetID        meshAssetId = HashAssetID(FormatTo(strBuf, "lightning_mesh_{}", boltEntity.index));
    const MaterialID     matAssetId  = HashAssetID(FormatTo(strBuf, "lightning_mat_{}", boltEntity.index));

    rc.RegisterMesh(meshAssetId, Mesh {.posBuffer = vboPos, .tangentFrameBuffer = vboFrame, .surfaceBuffer = vboSurface, .vertexCount = 0});

    if (auto matRes =
            rc.CreateMaterial({.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}})) {
        rc.RegisterMaterial(matAssetId, *matRes);
    }

    const FVec3 flashPos  = cameraPosition + FVec3{0.0f, 25.0f, 0.0f};
    const FVec3 impactPos = groundTarget + FVec3{0.0f, 20.0f, 0.0f};

    const Entity flashLight = reg.Create(
        Components::TransformComponent {.position = flashPos}, Components::HierarchyComponent {.parent = boltEntity},
        Components::LightComponent {
            .type        = LightType::Point,
            .color       = FVec3{0.88f, 0.95f, 1.0f},
            .intensity   = 0.0f,
            .radius      = 12.0f,
            .direction   = FVec3{0.0f, -1.0f, 0.0f},
            .range       = 2000.0f
        }
    );

    const Entity impactLight = reg.Create(
        Components::TransformComponent {.position = impactPos}, Components::HierarchyComponent {.parent = boltEntity},
        Components::LightComponent {
            .type        = LightType::Point,
            .color       = FVec3{1.0f, 1.0f, 1.0f},
            .intensity   = 0.0f,
            .radius      = 8.0f,
            .direction   = FVec3{0.0f, 1.0f, 0.0f},
            .range       = 500.0f
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
    auto rc = engine.Graphics();
    auto&      reg  = ZHLN::EngineAccess::RegistryOf(engine);
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
        bool needsMeshRegistration = !rc.Mesh(bolt.meshAssetId).has_value();
        if (bolt.vboPos == BufferID::Invalid || bolt.vboFrame == BufferID::Invalid || bolt.vboSurface == BufferID::Invalid) {
            const auto camComp = reg.GetSingleton<Components::CameraComponent>();
            if (!camComp) {
                // No view to face: the ribbon cannot be rebuilt, and the handles stay
                // invalid so the first frame that has a camera retries them. Building
                // it against a fabricated pose would bake the wrong geometry in.
                continue;
            }
            static thread_local std::mt19937 rng(std::random_device {}());
            const auto                       segments = GenerateFractalSegments(bolt.cloudOrigin, bolt.groundTarget, bolt.config.ribbonWidth, bolt.config, rng);
            const FVec3                  cameraPosition = camComp->camera.position;
            const auto                       ribbon   = BuildCameraFacingRibbon(segments, cameraPosition);
            if (bolt.vboPos == BufferID::Invalid) {
                bolt.vboPos = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.positions});
            }
            if (bolt.vboFrame == BufferID::Invalid) {
                bolt.vboFrame = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.frames});
            }
            if (bolt.vboSurface == BufferID::Invalid) {
                bolt.vboSurface = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.surfaces});
            }
            bolt.maxVertices      = ribbon.maxVertices;
            bolt.visibleVertices  = std::min(bolt.visibleVertices, bolt.maxVertices);
            needsMeshRegistration = true;
        }
        if (needsMeshRegistration && bolt.vboPos != BufferID::Invalid && bolt.vboFrame != BufferID::Invalid &&
            bolt.vboSurface != BufferID::Invalid) {
            rc.RegisterMesh(
                bolt.meshAssetId,
                Mesh {.posBuffer = bolt.vboPos, .tangentFrameBuffer = bolt.vboFrame, .surfaceBuffer = bolt.vboSurface, .vertexCount = bolt.visibleVertices}
            );
        }
        if (!rc.Material(bolt.matAssetId)) {
            if (auto mat = rc.CreateMaterial(
                    {.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}}
                )) {
                rc.RegisterMaterial(bolt.matAssetId, *mat);
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

                    engine.Audio().PostEvent(
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

        if (auto gpuMatOpt = rc.Material(bolt.matAssetId)) {
            Material    mat       = *gpuMatOpt;
            const float intensity = (bolt.phase == LightningPhase::SteppedLeader) ? 15.0f : (bolt.flashLuminance * bolt.config.emissiveIntensity);

            // Lanes 0-2 only: the fourth lane belongs to the ABI's emissive
            // packing and this path never owned it.
            mat.emissiveFactor.x  = 0.88f * intensity;
            mat.emissiveFactor.y  = 0.95f * intensity;
            mat.emissiveFactor.z  = 1.00f * intensity;
            rc.RegisterMaterial(bolt.matAssetId, mat);
        }

        if (auto gpuMeshOpt = rc.Mesh(bolt.meshAssetId)) {
            Mesh m        = *gpuMeshOpt;
            m.vertexCount = bolt.visibleVertices;
            rc.RegisterMesh(bolt.meshAssetId, m);
        }

        if (reg.IsAlive(bolt.flashLightEntity)) {
            reg.Patch<Components::LightComponent>(bolt.flashLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 8000000.0f; });
        }
        if (reg.IsAlive(bolt.impactLightEntity)) {
            reg.Patch<Components::LightComponent>(bolt.impactLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 4000000.0f; });
        }
    }

    const auto setAmbientExposure = [&engine](float value) {
        GraphicsSettings gfx            = engine.Graphics().Settings();
        gfx.environment.ambientExposure = value;
        engine.Graphics().ApplySettings(gfx);
    };
    if (hasActiveBolts) {
        setAmbientExposure(unflashedBaseExposure + (180.0f * peakLuminanceThisFrame));
    }

    for (const Entity deadEnt: deadEntities) {
        // The scene cleanup pass releases VBOs and child lights after the
        // simulation step, while the LightningComponent is still inspectable.
        DespawnEntity(engine, deadEnt);
    }

    if (!hasActiveBolts) {
        setAmbientExposure(unflashedBaseExposure);
    }
}

} // namespace Lightning

} // namespace ZHLN

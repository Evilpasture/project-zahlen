// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Zahlen/Engine/Capabilities/Entities.hpp>
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
#include <Zahlen/ECS/SystemParameters.hpp>
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

// These views are used inside the existing externally ordered Engine calls,
// not registered as parallel graph nodes. Graphics/audio and immediate
// hierarchy despawn remain phase-owned side effects.

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

void ReleaseLightning(GraphicsDevice render, LightningComponent& bolt) {
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
    auto entities = engine.Entities();
    auto render = engine.Graphics();
    for (const Entity e: entities.EntitiesWith<LightningComponent>()) {
        if (all || entities.Get<Components::PendingDestroy>(e)) {
            entities.Patch<LightningComponent>(e, [&](LightningComponent& bolt) { ReleaseLightning(render, bolt); });
        }
    }
}

void RegisterCleanup(Engine& engine) {
    engine.Entities().Register<LightningComponent>("LightningComponent");
    if (engine.AddSceneCleanupPass(&CleanupLightning)) {
        engine.AddDeviceLostCallback([](Engine& owner) {
            auto entities = owner.Entities();
            for (const Entity e: entities.EntitiesWith<LightningComponent>()) {
                entities.Patch<LightningComponent>(e, [](LightningComponent& bolt) {
                    bolt.vboPos = BufferID::Invalid;
                    bolt.vboFrame = BufferID::Invalid;
                    bolt.vboSurface = BufferID::Invalid;
                });
            }
        });
    }
}

} // namespace

namespace Lightning {

void Detach(Engine& engine, Entity entity) {
    auto entities = engine.Entities();
    auto render = engine.Graphics();
    if (entities.Patch<LightningComponent>(entity, [&](LightningComponent& bolt) { ReleaseLightning(render, bolt); })) {
        entities.Detach<LightningComponent>(entity);
    }
}

void Attach(Engine& engine, Entity entity, LightningComponent component) {
    RegisterCleanup(engine);
    Detach(engine, entity);
    engine.Entities().Attach(entity, std::move(component));
}

auto Spawn(Engine& engine, ZHLN::DVec3 cloudPos, ZHLN::DVec3 groundPos, const LightningConfig& cfg) -> Entity {
    auto entities = engine.Entities();
    auto rc = engine.Graphics();

    RegisterCleanup(engine);

    float      baseExposure = 4.5f;
    const auto existingEnts = entities.EntitiesWith<LightningComponent>();
    if (!existingEnts.empty()) {
        if (const auto existingComp = entities.Get<LightningComponent>(existingEnts[0])) {
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
    const auto camComp = entities.GetSingleton<Components::CameraComponent>();
    if (!camComp) {
        return Entity::Null();
    }

    const auto segments = GenerateFractalSegments(cloudOrigin, groundTarget, cfg.ribbonWidth, cfg, s_rng);
    const FVec3 cameraPosition = camComp->camera.position;
    const auto      ribbon         = BuildCameraFacingRibbon(segments, cameraPosition);

    const BufferID vboPos     = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.positions});
    const BufferID vboFrame   = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.frames});
    const BufferID vboSurface = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.surfaces});

    const Entity boltEntity = entities.Create();

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

    const Entity flashLight = entities.Spawn(
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

    const Entity impactLight = entities.Spawn(
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

    entities.Attach(
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

void RebuildBuffers(LightningComponent& bolt, GraphicsDevice rc, FVec3 cameraPosition) {
    static thread_local std::mt19937 rng(std::random_device {}());
    const auto segments = GenerateFractalSegments(bolt.cloudOrigin, bolt.groundTarget, bolt.config.ribbonWidth, bolt.config, rng);
    const auto ribbon = BuildCameraFacingRibbon(segments, cameraPosition);
    if (bolt.vboPos == BufferID::Invalid) bolt.vboPos = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.positions});
    if (bolt.vboFrame == BufferID::Invalid) bolt.vboFrame = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.frames});
    if (bolt.vboSurface == BufferID::Invalid) bolt.vboSurface = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.surfaces});
    bolt.maxVertices = ribbon.maxVertices;
    bolt.visibleVertices = std::min(bolt.visibleVertices, bolt.maxVertices);
}

} // namespace Lightning
} // namespace ZHLN

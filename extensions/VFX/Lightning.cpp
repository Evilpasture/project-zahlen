// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include "LightningGeometry.hpp"
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

    const FVec3 cameraPosition = camComp->camera.position;
    const auto  ribbon         = LightningHost::GenerateRibbon(
        cloudOrigin, groundTarget, cameraPosition, {.eta = cfg.eta, .ribbonWidth = cfg.ribbonWidth, .subdivisions = cfg.subdivisions}, s_rng
    );

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
    const auto                       ribbon = LightningHost::GenerateRibbon(
        bolt.cloudOrigin, bolt.groundTarget, cameraPosition,
        {.eta = bolt.config.eta, .ribbonWidth = bolt.config.ribbonWidth, .subdivisions = bolt.config.subdivisions}, rng
    );
    if (bolt.vboPos == BufferID::Invalid) bolt.vboPos = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.positions});
    if (bolt.vboFrame == BufferID::Invalid) bolt.vboFrame = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.frames});
    if (bolt.vboSurface == BufferID::Invalid) bolt.vboSurface = rc.CreateBuffer<BufferUsage::Vertex>(std::span {ribbon.surfaces});
    bolt.maxVertices = ribbon.maxVertices;
    bolt.visibleVertices = std::min(bolt.visibleVertices, bolt.maxVertices);
}

} // namespace Lightning
} // namespace ZHLN

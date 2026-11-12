// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// --- Global Module Fragment: Preprocessor Directives Only ---
#include <Zahlen/Common.h>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <cstdint>

export module ZHLN.Lightning;

namespace ZHLN {

export enum class LightningPhase: uint8_t { Idle, SteppedLeader, ReturnStroke, Dissipating };

export struct LightningConfig {
    float eta               = 2.0f;
    float peakCurrentKA     = 45.0f;
    float timeDilation      = 1.0f;
    float ribbonWidth       = 0.8f;
    int   subdivisions      = 5;
    float soundVolume       = 10.0f;
    float emissiveIntensity = 8000.0f;
};

export struct LightningComponent {
    LightningConfig config {};
    LightningPhase  phase = LightningPhase::Idle;

    float realTime       = 0.0f;
    float phaseTime      = 0.0f;
    float currentKA      = 0.0f;
    float flashLuminance = 0.0f;

    FVec3 cloudOrigin  = FVec3::Zero();
    FVec3 groundTarget = FVec3::Zero();

    BufferID vboPos          = BufferID::Invalid;
    BufferID vboFrame        = BufferID::Invalid;
    BufferID vboSurface      = BufferID::Invalid;
    AssetID      meshAssetId     = InvalidAssetID;
    MaterialID   matAssetId      = InvalidMaterialID;
    uint32_t     maxVertices     = 0;
    uint32_t     visibleVertices = 0;

    Entity flashLightEntity  = Entity::Null();
    Entity impactLightEntity = Entity::Null();

    float baseAmbientExposure = 4.5f;
};

namespace Lightning {

export auto Spawn(Engine& engine, ZHLN::DVec3 cloudPos, ZHLN::DVec3 groundPos, const LightningConfig& cfg = {}) -> Entity;

// Rendering support shared with the resolved update implementation.
// The update itself is declared in <VFX/LightningUpdate.hpp>.
export void RebuildBuffers(LightningComponent& bolt, GraphicsDevice graphics, FVec3 cameraPosition);

// Explicit replacement/removal of a resource-owning LightningComponent.
export void Attach(Engine& engine, Entity entity, LightningComponent component);
export void Detach(Engine& engine, Entity entity);

} // namespace Lightning

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// --- Global Module Fragment: Preprocessor Directives Only ---
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>
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

    JPH::Vec3 cloudOrigin  = JPH::Vec3::sZero();
    JPH::Vec3 groundTarget = JPH::Vec3::sZero();

    BufferHandle vboPos          = BufferHandle::Invalid;
    BufferHandle vboAttr         = BufferHandle::Invalid;
    AssetID      meshAssetId     = InvalidAssetID;
    MaterialID   matAssetId      = InvalidMaterialID;
    uint32_t     maxVertices     = 0;
    uint32_t     visibleVertices = 0;

    Entity flashLightEntity  = NullEntity;
    Entity impactLightEntity = NullEntity;

    float baseAmbientExposure = 4.5f;

    static void OnDestroy(LightningComponent* c) noexcept;
};

namespace Lightning {

export auto Spawn(Engine& engine, JPH::RVec3Arg cloudPos, JPH::RVec3Arg groundPos, const LightningConfig& cfg = {}) -> Entity;

export auto Update(Engine& engine, float dt) -> void;

} // namespace Lightning

} // namespace ZHLN

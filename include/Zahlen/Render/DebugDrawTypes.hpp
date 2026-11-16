// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixed-layout authoring values. No frame containers or backend headers.
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/ParticleEmitterDesc.hpp>

namespace ZHLN {
struct BillboardQuad {
    ZHLN::FVec3        position {};
    float             size     = 1.0f;
    float             rotation = 0.0f;
    ZHLN::FVec4        color {1.0f, 1.0f, 1.0f, 1.0f};
    ParticleAlignment facing = ParticleAlignment::CameraBillboard;
    ZHLN::FVec3        velocity {};
};

struct LineSegment {
    ZHLN::FVec3 start {};
    ZHLN::FVec3 end {};
    ZHLN::FVec4 colorStart {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::FVec4 colorEnd {1.0f, 1.0f, 1.0f, 1.0f};
};

struct ParticleEmitterSubmission {
    uint64_t             emitterId    = 0;
    uint32_t             maxParticles = 0;
    ParticleEmitterDesc  desc {};
    TextureID        texture  = TextureID::Invalid;
    bool                 additive = false;
};

struct MeshParticleEmitterSubmission {
    uint64_t                 emitterId    = 0;
    uint32_t                 maxParticles = 0;
    MeshParticleEmitterDesc  desc {};
    AssetID                  mesh = InvalidAssetID;
    MaterialID               mat  = InvalidMaterialID;
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Render/DebugDrawTypes.hpp>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<BillboardQuad> && std::is_trivially_copyable_v<BillboardQuad>);
static_assert(std::is_standard_layout_v<ParticleEmitterSubmission> && std::is_trivially_copyable_v<ParticleEmitterSubmission>);
static_assert(std::is_standard_layout_v<MeshParticleEmitterSubmission> && std::is_trivially_copyable_v<MeshParticleEmitterSubmission>);

// Submissions are copied into the engine's per-frame draw list during the call.
// Span storage need only live through the call; no caller allocations are kept.
struct IDebugDraw : IObject {
    virtual void AddLine(const FVec3& start, const FVec3& end,
                         const FVec4& colorStart, const FVec4& colorEnd) noexcept = 0;
    virtual Result<void> AddBillboards(TextureID texture, Span<const BillboardQuad> quads,
                                                     bool additive) noexcept = 0;
    virtual Result<void> AddDebugTriangles(Span<const VertexPosition> positions,
                                                         Span<const VertexSurface> surfaces) noexcept = 0;
    // Supported nodes: ParticleEmitterSubmission / MeshParticleEmitterSubmission.
    // Each node includes the authored emitter desc plus stable identity and
    // draw resources; these are values, never SceneData or GPU internals.
    virtual Result<void> SubmitParticles(const ChainInNode* emitterChain) noexcept = 0;
    virtual Result<void> SubmitMeshParticles(const ChainInNode* emitterChain) noexcept = 0;
    virtual void Clear() noexcept = 0;

    void AddLine(const FVec3& start, const FVec3& end, const FVec4& color) noexcept {
        AddLine(start, end, color, color);
    }
    auto SubmitParticles(const ParticleEmitterSubmission& submission) noexcept -> Result<void> {
        return SubmitParticles(InChain(submission));
    }
    auto SubmitMeshParticles(const MeshParticleEmitterSubmission& submission) noexcept -> Result<void> {
        return SubmitMeshParticles(InChain(submission));
    }
protected:
    ~IDebugDraw() = default;
};
} // namespace ZHLN::COM

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Per-frame overlay geometry. A frame's draw list is owned by the engine; the
// caller describes what to draw, it does not borrow a mutable scene container.

#include <Zahlen/Common.h>
#include <Zahlen/Render/RenderIDs.hpp>
#include <span>

namespace ZHLN {

class Float3;
class Float4;
struct SceneData;
struct BillboardQuad;
struct ParticleEmitterSubmission;
struct MeshParticleEmitterSubmission;
struct VertexPosition;
struct VertexSurface;

class ZHLN_API DebugDraw {
  public:
    void AddLine(const Float3& start, const Float3& end, const Float4& color);
    void AddLine(const Float3& start, const Float3& end, const Float4& colorStart, const Float4& colorEnd);
    void AddBillboards(TextureID texture, std::span<const BillboardQuad> quads, bool additive);
    void AddDebugTriangles(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces);
    void Clear() noexcept;
    void SetFrameDelta(float dt) noexcept;
    void SubmitParticles(const ParticleEmitterSubmission& submission);
    void SubmitMeshParticles(const MeshParticleEmitterSubmission& submission);


  private:
    friend class Engine;
    explicit DebugDraw(SceneData& scene) noexcept: _scene(&scene) {
    }
    SceneData* _scene;
};

} // namespace ZHLN

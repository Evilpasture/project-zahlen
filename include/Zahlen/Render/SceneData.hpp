// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// CPU-side frame description. ECS systems fill this before BeginFrame;
// FrameScope consumes it after GPU sync. No Vulkan, no RenderContext.
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <span>

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

struct BillboardBatch {
    TextureID         texture  = TextureID::Invalid;
    Array<BillboardQuad>  quads;
    bool                  additive = false;
};

// One 3D view's GPU inputs, packed for FrameScope::RenderScene.
struct SceneRenderPass {
    SceneView        view {};
    Camera           camera {};
    FrameData        frame {};
    ZHLN::Mat4x4      shadowProjView = ZHLN::Mat4x4::Identity();
    GraphicsSettings settings {};
};

struct SceneData {
    float                                dt = 0.0166f;
    Array<LightDesc>                     lights;
    Array<LineSegment>                   lines;
    Array<VertexPosition>                debugTriPositions;
    Array<VertexSurface>                 debugTriSurfaces;
    Array<ParticleEmitterSubmission>     particleEmitters;
    Array<MeshParticleEmitterSubmission> meshParticleEmitters;
    Array<BillboardBatch>                billboards;

    void Clear() noexcept {
        lights.clear();
        lines.clear();
        debugTriPositions.clear();
        debugTriSurfaces.clear();
        particleEmitters.clear();
        meshParticleEmitters.clear();
        billboards.clear();
    }

    void AddLine(ZHLN::FVec3 start, ZHLN::FVec3 end, ZHLN::FVec4 colorStart, ZHLN::FVec4 colorEnd) {
        lines.push_back({start, end, colorStart, colorEnd});
    }
    void AddLine(ZHLN::FVec3 start, ZHLN::FVec3 end, ZHLN::FVec4 color) {
        AddLine(start, end, color, color);
    }

    void AddBillboards(TextureID texture, std::span<const BillboardQuad> quads, bool additive) {
        if (quads.empty()) {
            return;
        }
        BillboardBatch batch;
        batch.texture  = texture;
        batch.additive = additive;
        batch.quads.append(quads);
        billboards.push_back(std::move(batch));
    }

    void AddDebugTriangles(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces) {
        if (positions.size() != surfaces.size() || positions.empty()) {
            return;
        }
        debugTriPositions.append(positions);
        debugTriSurfaces.append(surfaces);
    }
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// CPU-side frame description. ECS systems fill this before BeginFrame;
// FrameScope consumes it after GPU sync. No Vulkan, no RenderContext.
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Vertex.hpp>
#include <span>

namespace ZHLN {

struct BillboardQuad {
    JPH::Vec3         position {};
    float             size     = 1.0f;
    float             rotation = 0.0f;
    JPH::Vec4         color {1.0f, 1.0f, 1.0f, 1.0f};
    ParticleAlignment facing = ParticleAlignment::CameraBillboard;
    JPH::Vec3         velocity {};
};

struct LineSegment {
    JPH::Vec3 start {};
    JPH::Vec3 end {};
    JPH::Vec4 colorStart {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Vec4 colorEnd {1.0f, 1.0f, 1.0f, 1.0f};
};

struct ParticleEmitterSubmission {
    uint64_t             emitterId    = 0;
    uint32_t             maxParticles = 0;
    ParticleEmitterDesc  desc {};
    TextureHandle        texture  = TextureHandle::Invalid;
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
    TextureHandle         texture  = TextureHandle::Invalid;
    Array<BillboardQuad>  quads;
    bool                  additive = false;
};

// One 3D view's GPU inputs, packed for FrameScope::RenderScene.
struct SceneRenderPass {
    SceneView        view {};
    Camera           camera {};
    FrameData        frame {};
    JPH::Mat44       shadowProjView = JPH::Mat44::sIdentity();
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

    void AddLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) {
        lines.push_back({start, end, colorStart, colorEnd});
    }
    void AddLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) {
        AddLine(start, end, color, color);
    }

    void AddBillboards(TextureHandle texture, std::span<const BillboardQuad> quads, bool additive) {
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

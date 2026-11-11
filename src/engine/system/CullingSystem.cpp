// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include "CullingSystem.hpp"
#include <Zahlen/Engine/VisibilityList.hpp>
#include "LightingSystem.hpp"
#include "Zahlen/Render/Render.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/ECS/ECS.hpp>

namespace ZHLN::Tests { namespace {
void VerifyCullingResults(CullingSystem::CullingQuery query, std::span<const Entity> visible, const Frustum& frustum, const CullingStats& stats) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }
    testsRun = true;

    auto entities = query.Entities<Components::MeshComponent>();
    auto meshes   = query.Raw<Components::MeshComponent>();

    size_t expectedVisible = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
        if ((meshes[i].flags & DrawFlags::Hidden) != DrawFlags::None) {
            continue;
        }

        const auto worldTrans = query.Get<Components::WorldTransformComponent>(entities[i]);
        auto worldMat = Mat4x4::Identity();
        if (worldTrans) {
            worldMat = worldTrans->world;
        }
        FVec3  pos        = TransformPoint(worldMat, meshes[i].localCenter);

        float currentMaxScale = std::max({worldMat.Column(0).XYZ().Length(), worldMat.Column(1).XYZ().Length(), worldMat.Column(2).XYZ().Length()});

        if (frustum.IsSphereVisible(pos, meshes[i].cullRadius * currentMaxScale)) {
            expectedVisible++;
        }
    }

    if (visible.size() > entities.size()) {
        ZHLN::Log("[Test Fail] Culling: Visible count {} exceeds total entity count {}", visible.size(), entities.size());
    }

    for (Entity e: visible) {
        if (!query.IsAlive(e)) {
            ZHLN::Log("[Test Fail] Culling: Visible list contains dead entity {}", e.index);
        }
    }

    if (visible.size() != expectedVisible && stats.EnableCulling) {
        ZHLN::Log("[Test Fail] Culling: Visible count {} does not match expected {}", visible.size(), expectedVisible);
    }
}
}}

namespace ZHLN {

namespace {

[[nodiscard]] inline float GetLane(const FVec4& v, uint32_t lane) noexcept {
    switch (lane) {
        case 0:  return v.x;
        case 1:  return v.y;
        case 2:  return v.z;
        default: return v.w;
    }
}

struct BatchedFrustum {
    static constexpr uint32_t kPlaneCount = 8;

    std::array<float, kPlaneCount> nx {};
    std::array<float, kPlaneCount> ny {};
    std::array<float, kPlaneCount> nz {};
    std::array<float, kPlaneCount> pw {};

    [[nodiscard]] static BatchedFrustum FromFrustum(const Frustum& frustum) noexcept {
        BatchedFrustum out;
        for (uint32_t p = 0; p < kPlaneCount; ++p) {
            const uint32_t block = (p < 4) ? 0 : 1;
            const uint32_t lane  = p & 3;
            out.nx[p]            = GetLane(frustum.mX[block], lane);
            out.ny[p]            = GetLane(frustum.mY[block], lane);
            out.nz[p]            = GetLane(frustum.mZ[block], lane);
            out.pw[p]            = GetLane(frustum.mW[block], lane);
        }
        return out;
    }

    void Test4(const FVec4& centersX, const FVec4& centersY, const FVec4& centersZ, const FVec4& negInflatedRadii, bool* outVisible) const noexcept {
        FVec4 worstViolation = FVec4::Splat(-1.0f);

        for (uint32_t p = 0; p < kPlaneCount; ++p) {
            FVec4 dist = FVec4::Splat(nx[p]) * centersX + FVec4::Splat(ny[p]) * centersY + FVec4::Splat(nz[p]) * centersZ +
                             FVec4::Splat(pw[p]);
            const FVec4 violation = negInflatedRadii - dist;
            worstViolation = {std::max(worstViolation.x, violation.x), std::max(worstViolation.y, violation.y),
                              std::max(worstViolation.z, violation.z), std::max(worstViolation.w, violation.w)};
        }

        for (uint32_t j = 0; j < 4; ++j) {
            outVisible[j] = GetLane(worstViolation, j) <= 0.0f;
        }
    }
};

}


void CullingSystem::GraphUpdate(CullingQuery query, ECS::Res<RenderContext> render, VisibleEntities visible, VisibleShadowEntities shadow,
                                ECS::Local<CullingScratch> scratch) {
    // The pose the culler tests against is the main camera entity's: it is world
    // data, not a service. No camera entity means this frame has no view to cull
    // for -- RenderSystem reports NoMainCamera for it.
    auto camComp = query.GetSingleton<Components::CameraComponent>();
    if (!camComp) {
        return;
    }

    // The counters are world data, so they live in the registry rather than on
    // the pass. World::Create() creates the singleton and a scene reset re-seeds it
    // in InitializeDefaultScene, so this is unreachable on those paths -- it marks the
    // one that is left: a graph run against a registry that was cleared and never
    // re-seeded. (World::GetCullingStats() would create it, but the culler reaches the
    // component through its declared query, which by design cannot emplace.)
    auto stats = query.GetSingleton<Components::CullingStatsComponent>();
    ZHLN::Assert(stats.has_value(), "CullingStatsComponent singleton is missing: World::Create() creates it and InitializeDefaultScene re-seeds it after a scene clear");

    UpdateCore(query, *render, camComp->camera, true, *scratch, stats->stats, visible.values, shadow.values);
}

void CullingSystem::UpdateCore(CullingQuery query, const RenderContext& rc, const Camera& cam, bool engineCam, CullingScratch& scratch,
                               CullingStats& stats, VisibilityList& outVisible, VisibilityList& outVisibleShadow) {
    ZHLN::ScopedTimer profTimer("Culling (ECS O(N))");

    auto entities       = query.Entities<Components::MeshComponent>();
    auto cameraEntities = query.Entities<Components::CameraComponent>();

    ZHLN::Optional<Components::CameraComponent&> cComp;
    if (!cameraEntities.empty()) {
        cComp = query.Get<Components::CameraComponent>(cameraEntities[0]);
    }

    // Pipeline settings come from the context that owns them, not from a
    // registry entity the pass has to go hunting for.
    const GraphicsSettings& gfx              = rc.GetSettings();
    const bool              isFullBright     = (gfx.environment.fullBright != 0);
    const float             shadowWidth      = gfx.shadows.width;
    const uint32_t          shadowResolution = gfx.shadows.resolution;

    if (stats.FreezeFrustum && engineCam) {
        if (!scratch.wasFrozen) {
            if (cComp) {
                cComp->frozenViewProj  = cComp->unjitteredViewProj;
                scratch.frustumCorners = FrustumCornersFromViewProj(cComp->frozenViewProj);
            }
            scratch.wasFrozen = true;
        }
        if (cComp) {
            scratch.mainFrustum.Update(cComp->frozenViewProj);
        }
    } else {
        if (engineCam && cComp) {
            scratch.mainFrustum.Update(cComp->unjitteredViewProj);
        }
        if (!stats.FreezeFrustum) {
            scratch.wasFrozen = false;
        }
    }

    if (!isFullBright) {
        auto [sunDirection, sunIntensity] = LightingSystem::GetSunDirectionAndIntensity(
            query.Select<const Components::LightComponent, const Components::WorldTransformComponent,
                         const Components::TransformComponent, const Components::SunTagComponent,
                         const Components::EnvironmentSunTagComponent>());

        FVec3 shadowCenter = cam.position;
        float     texelSize    = shadowWidth / static_cast<float>(shadowResolution);
        shadowCenter.x = std::round(shadowCenter.x / texelSize) * texelSize;
        shadowCenter.y = std::round(shadowCenter.y / texelSize) * texelSize;
        shadowCenter.z = std::round(shadowCenter.z / texelSize) * texelSize;

        FVec3  lightPos  = shadowCenter + sunDirection * Shadows::FarOffset;
        const auto lightView = CreateLookAt(lightPos, shadowCenter, {0.0f, 1.0f, 0.0f});

        float      halfWidth      = shadowWidth * 0.5f;
        const auto lightProj = CreateOrtho(-halfWidth, halfWidth, -halfWidth, halfWidth, Shadows::NearClip, Shadows::FarDepth);
        const auto shadowProjView = lightProj * lightView;

        scratch.shadowFrustum.Update(shadowProjView);
    }

    auto meshes = query.Raw<Components::MeshComponent>();

    stats.TotalTriangles    = 0;
    stats.RenderedTriangles = 0;

    if (!stats.EnableCulling) {
        outVisible.Clear();
        outVisible.AddRange(entities);
        outVisibleShadow.Clear();
        outVisibleShadow.AddRange(entities);

        uint32_t tris = 0;
        for (size_t i = 0; i < entities.size(); ++i) {
            auto gpuMeshOpt = rc.GetGPUMesh(meshes[i].meshAsset);
            if (gpuMeshOpt.has_value()) {
                tris += (gpuMeshOpt->indexCount > 0) ? (gpuMeshOpt->indexCount / 3) : (gpuMeshOpt->vertexCount / 3);
            }
        }
        stats.TotalTriangles    = tris;
        stats.RenderedTriangles = tris;
        return;
    }

    outVisible.Clear();
    outVisibleShadow.Clear();

    constexpr size_t     kBatch = 4;
    const BatchedFrustum mainPlanes   = BatchedFrustum::FromFrustum(scratch.mainFrustum);
    const BatchedFrustum shadowPlanes = BatchedFrustum::FromFrustum(scratch.shadowFrustum);

    std::array<Entity, kBatch>  batchEntities {};
    std::array<FVec3, kBatch> batchCenters {};
    std::array<float, kBatch>   batchRadii {};
    std::array<float, kBatch>   batchScaledRadii {};
    std::array<uint32_t, kBatch> batchTris {};
    std::array<bool, kBatch>    batchHidden {};
    std::array<bool, kBatch>    batchBackground {};
    std::array<bool, 4>         mainVisible {};
    std::array<bool, 4>         shadowVisible {};

    size_t i = 0;
    while (i < entities.size()) {
        size_t n = 0;
        while ((i < entities.size()) && (n < kBatch)) {
            const Entity e          = entities[i];
            const auto&  meshComp   = meshes[i];
            const bool   hidden     = (meshComp.flags & DrawFlags::Hidden) != DrawFlags::None;
            auto         gpuMeshOpt = rc.GetGPUMesh(meshComp.meshAsset);
            uint32_t     meshTris   = 0;
            if (gpuMeshOpt.has_value()) {
                meshTris = (gpuMeshOpt->indexCount > 0) ? (gpuMeshOpt->indexCount / 3) : (gpuMeshOpt->vertexCount / 3);
            }

            const auto worldTrans = query.Get<Components::WorldTransformComponent>(e);
            auto worldMat = Mat4x4::Identity();
            if (worldTrans) {
                worldMat = worldTrans->world;
            }

            batchEntities[n] = e;
            batchCenters[n]  = TransformPoint(worldMat, meshComp.localCenter);
            batchRadii[n]    = meshComp.cullRadius;
            batchScaledRadii[n] =
                meshComp.cullRadius * std::max({worldMat.Column(0).XYZ().Length(), worldMat.Column(1).XYZ().Length(), worldMat.Column(2).XYZ().Length()});
            batchTris[n]   = meshTris;
            batchHidden[n] = hidden;
            // Background scenery is culled like everything else now (it lives
            // at its authored place in the same world). The flag is kept for
            // one rule: a backdrop must never cast a cascade shadow -- its
            // ground plane is hundreds of metres wide and sits inside the
            // cascades' reach, so letting it cast would blacken the level.
            batchBackground[n] = (meshComp.flags & DrawFlags::Background) != DrawFlags::None;

            stats.TotalTriangles += hidden ? 0u : meshTris;

            ++i;
            ++n;
        }

        const FVec4 centersX(batchCenters[0].x, batchCenters[1].x, batchCenters[2].x, batchCenters[3].x);
        const FVec4 centersY(batchCenters[0].y, batchCenters[1].y, batchCenters[2].y, batchCenters[3].y);
        const FVec4 centersZ(batchCenters[0].z, batchCenters[1].z, batchCenters[2].z, batchCenters[3].z);
        const FVec4 negScaled = FVec4(
            -(batchScaledRadii[0] + 0.5f), -(batchScaledRadii[1] + 0.5f), -(batchScaledRadii[2] + 0.5f), -(batchScaledRadii[3] + 0.5f)
        );

        mainPlanes.Test4(centersX, centersY, centersZ, negScaled, mainVisible.data());

        if (!isFullBright) {
            const FVec4 negPlain = FVec4(-(batchRadii[0] + 0.5f), -(batchRadii[1] + 0.5f), -(batchRadii[2] + 0.5f), -(batchRadii[3] + 0.5f));
            shadowPlanes.Test4(centersX, centersY, centersZ, negPlain, shadowVisible.data());
        }

        for (size_t j = 0; j < n; ++j) {
            if (batchHidden[j]) {
                continue;
            }

            if (mainVisible[j]) {
                outVisible.Add(batchEntities[j]);
                stats.RenderedTriangles += batchTris[j];
            }

            // A backdrop never casts a cascade shadow (see batchBackground).
            if (!isFullBright && shadowVisible[j] && !batchBackground[j]) {
                outVisibleShadow.Add(batchEntities[j]);
            }
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyCullingResults(query, outVisible.AsSpan(), scratch.mainFrustum, stats);
    }
}

void CullingSystem::DrawDebugFrustum(Engine& engine) {
    auto& reg    = ZHLN::EngineAccess::RegistryOf(engine);
    auto  stats  = reg.GetSingleton<Components::CullingStatsComponent>();
    if (!stats || !stats->stats.FreezeFrustum) {
        return;
    }

    // The pass's corners are its own state, so this draws the same frustum from
    // the same source: the view-projection the pass froze.
    const auto camComp = reg.GetSingleton<Components::CameraComponent>();
    if (!camComp) {
        return;
    }
    const std::array<FVec3, 8> corners = FrustumCornersFromViewProj(camComp->frozenViewProj);

    auto scene = engine.Debug();

    struct FrustumEdge {
        int start;
        int end;
    };
    static constexpr std::array<FrustumEdge, 12> frustumEdges = {
        {{.start = 0, .end = 1},
         {.start = 1, .end = 2},
         {.start = 2, .end = 3},
         {.start = 3, .end = 0},
         {.start = 4, .end = 5},
         {.start = 5, .end = 6},
         {.start = 6, .end = 7},
         {.start = 7, .end = 4},
         {.start = 0, .end = 4},
         {.start = 1, .end = 5},
         {.start = 2, .end = 6},
         {.start = 3, .end = 7}}
    };

    FVec4 cyanColor(0.0f, 1.0f, 1.0f, 1.0f);
    for (auto edge: frustumEdges) {
        scene.AddLine(corners[edge.start], corners[edge.end], cyanColor, cyanColor);
    }
}

std::array<FVec3, 8> FrustumCornersFromViewProj(const Mat4x4& viewProj) noexcept {
    // FVec4's constructor is not constexpr, so this is a constant of the pass
    // rather than a compile-time one.
    const std::array<FVec4, 8> kClipCorners = {
        FVec4 {-1.0f, -1.0f, 0.0f, 1.0f},
        FVec4 {1.0f, -1.0f, 0.0f, 1.0f},
        FVec4 {1.0f, 1.0f, 0.0f, 1.0f},
        FVec4 {-1.0f, 1.0f, 0.0f, 1.0f},
        FVec4 {-1.0f, -1.0f, 1.0f, 1.0f},
        FVec4 {1.0f, -1.0f, 1.0f, 1.0f},
        FVec4 {1.0f, 1.0f, 1.0f, 1.0f},
        FVec4 {-1.0f, 1.0f, 1.0f, 1.0f},
    };

    std::array<FVec3, 8> corners {};
    corners.fill(FVec3::Zero());

    const auto invViewProj = Inverse(viewProj);
    for (size_t i = 0; i < kClipCorners.size(); ++i) {
        const FVec4 worldPos = invViewProj * kClipCorners[i];
        const float     w        = worldPos.w;
        if (std::abs(w) > 1e-6f) {
            corners[i] = FVec3(worldPos.x / w, worldPos.y / w, worldPos.z / w);
        }
    }
    return corners;
}
}

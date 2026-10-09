// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CullingSystem.hpp"
#include "LightingSystem.hpp"
#include "Zahlen/Render/Render.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN::Tests { namespace {
void VerifyCullingResults(CullingSystem::CullingQuery query, const JPH::Array<Entity>& visible, const Frustum& frustum, const CullingStats& stats) noexcept {
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
        JPH::Mat44 worldMat   = worldTrans ? worldTrans->world : JPH::Mat44::sIdentity();
        JPH::Vec3  pos        = worldMat * meshes[i].localCenter;

        float currentMaxScale = std::max({worldMat.GetColumn3(0).Length(), worldMat.GetColumn3(1).Length(), worldMat.GetColumn3(2).Length()});

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

[[nodiscard]] inline float GetLane(const JPH::Vec4& v, uint32_t lane) noexcept {
    switch (lane) {
        case 0:  return v.GetX();
        case 1:  return v.GetY();
        case 2:  return v.GetZ();
        default: return v.GetW();
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

    void Test4(const JPH::Vec4& centersX, const JPH::Vec4& centersY, const JPH::Vec4& centersZ, const JPH::Vec4& negInflatedRadii, bool* outVisible) const noexcept {
        JPH::Vec4 worstViolation = JPH::Vec4::sReplicate(-1.0f);

        for (uint32_t p = 0; p < kPlaneCount; ++p) {
            JPH::Vec4 dist = JPH::Vec4::sReplicate(nx[p]) * centersX + JPH::Vec4::sReplicate(ny[p]) * centersY + JPH::Vec4::sReplicate(nz[p]) * centersZ +
                             JPH::Vec4::sReplicate(pw[p]);
            worstViolation = JPH::Vec4::sMax(worstViolation, negInflatedRadii - dist);
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
                               CullingStats& stats, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow) {
    ZHLN::ScopedTimer profTimer("Culling (ECS O(N))");

    auto entities       = query.Entities<Components::MeshComponent>();
    auto cameraEntities = query.Entities<Components::CameraComponent>();

    ZHLN::Optional<Components::CameraComponent&> cComp;
    if (!cameraEntities.empty()) {
        cComp = query.Get<Components::CameraComponent>(cameraEntities[0]);
    }

    bool     isFullBright     = false;
    float    shadowWidth      = 80.0f;
    uint32_t shadowResolution = 2048;

    auto settingsEntities = query.Entities<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto pp = query.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
            isFullBright = (pp->fullBright != 0);
        }
    }

    auto shadowEntities = query.Entities<Components::ShadowSettingsComponent>();
    if (!shadowEntities.empty()) {
        if (auto shadowSettings = query.Get<Components::ShadowSettingsComponent>(shadowEntities[0])) {
            shadowWidth      = shadowSettings->shadowWidth;
            shadowResolution = shadowSettings->shadowResolution;
        }
    }

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

        JPH::Vec3 shadowCenter = cam.position;
        float     texelSize    = shadowWidth / static_cast<float>(shadowResolution);
        shadowCenter.SetX(std::round(shadowCenter.GetX() / texelSize) * texelSize);
        shadowCenter.SetY(std::round(shadowCenter.GetY() / texelSize) * texelSize);
        shadowCenter.SetZ(std::round(shadowCenter.GetZ() / texelSize) * texelSize);

        JPH::Vec3  lightPos  = shadowCenter + sunDirection * Shadows::FarOffset;
        JPH::Mat44 lightView = Math::CreateLookAt(lightPos, shadowCenter, JPH::Vec3::sAxisY());

        float      halfWidth      = shadowWidth * 0.5f;
        JPH::Mat44 lightProj      = Math::CreateOrtho(-halfWidth, halfWidth, -halfWidth, halfWidth, Shadows::NearClip, Shadows::FarDepth);
        JPH::Mat44 shadowProjView = lightProj * lightView;

        scratch.shadowFrustum.Update(shadowProjView);
    }

    auto meshes = query.Raw<Components::MeshComponent>();

    stats.TotalTriangles    = 0;
    stats.RenderedTriangles = 0;

    if (!stats.EnableCulling) {
        outVisible.assign(entities.begin(), entities.end());
        outVisibleShadow.assign(entities.begin(), entities.end());

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

    outVisible.clear();
    outVisibleShadow.clear();

    constexpr size_t     kBatch = 4;
    const BatchedFrustum mainPlanes   = BatchedFrustum::FromFrustum(scratch.mainFrustum);
    const BatchedFrustum shadowPlanes = BatchedFrustum::FromFrustum(scratch.shadowFrustum);

    std::array<Entity, kBatch>  batchEntities {};
    std::array<JPH::Vec3, kBatch> batchCenters {};
    std::array<float, kBatch>   batchRadii {};
    std::array<float, kBatch>   batchScaledRadii {};
    std::array<uint32_t, kBatch> batchTris {};
    std::array<bool, kBatch>    batchHidden {};
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
            JPH::Mat44 worldMat   = worldTrans ? worldTrans->world : JPH::Mat44::sIdentity();

            batchEntities[n] = e;
            batchCenters[n]  = worldMat * meshComp.localCenter;
            batchRadii[n]    = meshComp.cullRadius;
            batchScaledRadii[n] =
                meshComp.cullRadius * std::max({worldMat.GetColumn3(0).Length(), worldMat.GetColumn3(1).Length(), worldMat.GetColumn3(2).Length()});
            batchTris[n]   = meshTris;
            batchHidden[n] = hidden;

            stats.TotalTriangles += hidden ? 0u : meshTris;

            ++i;
            ++n;
        }

        const JPH::Vec4 centersX(batchCenters[0].GetX(), batchCenters[1].GetX(), batchCenters[2].GetX(), batchCenters[3].GetX());
        const JPH::Vec4 centersY(batchCenters[0].GetY(), batchCenters[1].GetY(), batchCenters[2].GetY(), batchCenters[3].GetY());
        const JPH::Vec4 centersZ(batchCenters[0].GetZ(), batchCenters[1].GetZ(), batchCenters[2].GetZ(), batchCenters[3].GetZ());
        const JPH::Vec4 negScaled = JPH::Vec4(
            -(batchScaledRadii[0] + 0.5f), -(batchScaledRadii[1] + 0.5f), -(batchScaledRadii[2] + 0.5f), -(batchScaledRadii[3] + 0.5f)
        );

        mainPlanes.Test4(centersX, centersY, centersZ, negScaled, mainVisible.data());

        if (!isFullBright) {
            const JPH::Vec4 negPlain = JPH::Vec4(-(batchRadii[0] + 0.5f), -(batchRadii[1] + 0.5f), -(batchRadii[2] + 0.5f), -(batchRadii[3] + 0.5f));
            shadowPlanes.Test4(centersX, centersY, centersZ, negPlain, shadowVisible.data());
        }

        for (size_t j = 0; j < n; ++j) {
            if (batchHidden[j]) {
                continue;
            }

            if (mainVisible[j]) {
                outVisible.push_back(batchEntities[j]);
                stats.RenderedTriangles += batchTris[j];
            }

            if (!isFullBright && shadowVisible[j]) {
                outVisibleShadow.push_back(batchEntities[j]);
            }
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyCullingResults(query, outVisible, scratch.mainFrustum, stats);
    }
}

void CullingSystem::DrawDebugFrustum(Engine& engine) {
    auto& reg    = engine.GetRegistry();
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
    const std::array<JPH::Vec3, 8> corners = FrustumCornersFromViewProj(camComp->frozenViewProj);

    auto& scene = engine.GetSceneData();

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

    JPH::Vec4 cyanColor(0.0f, 1.0f, 1.0f, 1.0f);
    for (auto edge: frustumEdges) {
        scene.AddLine(corners[edge.start], corners[edge.end], cyanColor, cyanColor);
    }
}

std::array<JPH::Vec3, 8> FrustumCornersFromViewProj(const JPH::Mat44& viewProj) noexcept {
    // JPH::Vec4's constructor is not constexpr, so this is a constant of the pass
    // rather than a compile-time one.
    const std::array<JPH::Vec4, 8> kClipCorners = {
        JPH::Vec4 {-1.0f, -1.0f, 0.0f, 1.0f},
        JPH::Vec4 {1.0f, -1.0f, 0.0f, 1.0f},
        JPH::Vec4 {1.0f, 1.0f, 0.0f, 1.0f},
        JPH::Vec4 {-1.0f, 1.0f, 0.0f, 1.0f},
        JPH::Vec4 {-1.0f, -1.0f, 1.0f, 1.0f},
        JPH::Vec4 {1.0f, -1.0f, 1.0f, 1.0f},
        JPH::Vec4 {1.0f, 1.0f, 1.0f, 1.0f},
        JPH::Vec4 {-1.0f, 1.0f, 1.0f, 1.0f},
    };

    std::array<JPH::Vec3, 8> corners {};
    corners.fill(JPH::Vec3::sZero());

    const JPH::Mat44 invViewProj = viewProj.Inversed();
    for (size_t i = 0; i < kClipCorners.size(); ++i) {
        const JPH::Vec4 worldPos = invViewProj * kClipCorners[i];
        const float     w        = worldPos.GetW();
        if (std::abs(w) > 1e-6f) {
            corners[i] = JPH::Vec3(worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w);
        }
    }
    return corners;
}
}

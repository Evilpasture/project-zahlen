// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CullingSystem.hpp"
#include "LightingSystem.hpp"
#include "Zahlen/Render/Render.hpp"
#include "CameraSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN::Tests { namespace {
void VerifyCullingResults(CullingSystem::CullingQuery query, const JPH::Array<Entity>& visible, const Camera& cam, const CullingStats& stats) noexcept {
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

        if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius * currentMaxScale)) {
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


template <bool UsePhysicsTransforms>
void CullingSystem::Update(Engine& engine, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow) {
    Update<UsePhysicsTransforms>(engine, engine.GetCamera(), outVisible, outVisibleShadow);
}

template <bool UsePhysicsTransforms>
void CullingSystem::Update(Engine& engine, Camera& cam, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow) {
    // Explicit parameters rather than a hand-built context: the graph path
    // (GraphUpdate) is the one the engine uses, and these entry points exist for
    // callers that hold the pieces already.
    UpdateCore<UsePhysicsTransforms>(CullingQuery(engine.GetRegistry()), engine.GetRenderContext(), cam, &cam == &engine.GetCamera(), outVisible,
                                     outVisibleShadow);
}

void CullingSystem::GraphUpdate(CullingQuery query, ECS::ResMut<CullingSystem> culling, ECS::Res<RenderContext> render,
                                ECS::ResMut<Camera> camera, VisibleEntities visible, VisibleShadowEntities shadow) {
    culling->UpdateCore<false>(query, *render, *camera, true, visible.values, shadow.values);
}

template <bool UsePhysicsTransforms>
void CullingSystem::UpdateCore(CullingQuery query, const RenderContext& rc, Camera& cam, bool engineCam,
                               JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow) {
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

    if (m_stats.FreezeFrustum && engineCam) {
        if (!m_wasFrozen) {
            if (cComp) {
                cComp->frozenViewProj = cComp->unjitteredViewProj;
                JPH::Mat44 invVP      = cComp->frozenViewProj.Inversed();
                auto       ndc        = std::to_array<JPH::Vec4>(
                    {{-1.0f, -1.0f, 0.0f, 1.0f},
                     {1.0f, -1.0f, 0.0f, 1.0f},
                     {1.0f, 1.0f, 0.0f, 1.0f},
                     {-1.0f, 1.0f, 0.0f, 1.0f},
                     {-1.0f, -1.0f, 1.0f, 1.0f},
                     {1.0f, -1.0f, 1.0f, 1.0f},
                     {1.0f, 1.0f, 1.0f, 1.0f},
                     {-1.0f, 1.0f, 1.0f, 1.0f}}
                );
                for (int i = 0; i < 8; ++i) {
                    JPH::Vec4 worldPos = invVP * ndc[i];
                    float     w        = worldPos.GetW();
                    if (std::abs(w) > 1e-6f) {
                        m_frustumCorners[i] = JPH::Vec3(worldPos.GetX() / w, worldPos.GetY() / w, worldPos.GetZ() / w);
                    }
                }
            }
            m_wasFrozen = true;
        }
        if (cComp) {
            cam.frustum.Update(cComp->frozenViewProj);
        }
    } else {
        if (engineCam && cComp) {
            cam.frustum.Update(cComp->unjitteredViewProj);
        }
        if (!m_stats.FreezeFrustum) {
            m_wasFrozen = false;
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

        cam.shadowFrustum.Update(shadowProjView);
    }

    auto meshes = query.Raw<Components::MeshComponent>();

    m_stats.TotalTriangles    = 0;
    m_stats.RenderedTriangles = 0;

    if (!m_stats.EnableCulling) {
        outVisible.assign(entities.begin(), entities.end());
        outVisibleShadow.assign(entities.begin(), entities.end());

        uint32_t tris = 0;
        for (size_t i = 0; i < entities.size(); ++i) {
            auto gpuMeshOpt = rc.GetGPUMesh(meshes[i].meshAsset);
            if (gpuMeshOpt.has_value()) {
                tris += (gpuMeshOpt->indexCount > 0) ? (gpuMeshOpt->indexCount / 3) : (gpuMeshOpt->vertexCount / 3);
            }
        }
        m_stats.TotalTriangles    = tris;
        m_stats.RenderedTriangles = tris;
        return;
    }

    outVisible.clear();
    outVisibleShadow.clear();

    constexpr size_t     kBatch = 4;
    const BatchedFrustum mainPlanes   = BatchedFrustum::FromFrustum(cam.frustum);
    const BatchedFrustum shadowPlanes = BatchedFrustum::FromFrustum(cam.shadowFrustum);

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

            m_stats.TotalTriangles += hidden ? 0u : meshTris;

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
                m_stats.RenderedTriangles += batchTris[j];
            }

            if (!isFullBright && shadowVisible[j]) {
                outVisibleShadow.push_back(batchEntities[j]);
            }
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyCullingResults(query, outVisible, cam, m_stats);
    }
}

void CullingSystem::DrawDebugFrustum(Engine& engine) {
    if (!m_stats.FreezeFrustum) {
        return;
    }

    auto& rc = engine.GetRenderContext();

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
        JPH::Vec3 pA = m_frustumCorners[edge.start];
        JPH::Vec3 pB = m_frustumCorners[edge.end];
        rc.DrawLine(pA, pB, cyanColor, cyanColor);
    }
}

template void CullingSystem::Update<true>(Engine&, JPH::Array<Entity>&, JPH::Array<Entity>&);
template void CullingSystem::Update<false>(Engine&, JPH::Array<Entity>&, JPH::Array<Entity>&);
template void CullingSystem::Update<true>(Engine&, Camera&, JPH::Array<Entity>&, JPH::Array<Entity>&);
template void CullingSystem::Update<false>(Engine&, Camera&, JPH::Array<Entity>&, JPH::Array<Entity>&);
}

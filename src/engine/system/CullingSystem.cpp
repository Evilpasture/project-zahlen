// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CullingSystem.hpp"
#include "LightingSystem.hpp"
#include "Zahlen/Render.hpp"
#include "engine/system/CameraSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <engine/graphics/Resources.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests { namespace {
void VerifyCullingResults(const ECS::Registry& reg, const JPH::Array<Entity>& visible, const Camera& cam) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }
    testsRun = true;

    auto entities = reg.GetEntitiesWith<Components::MeshComponent>();
    auto meshes   = reg.GetRawArray<Components::MeshComponent>();

    size_t expectedVisible = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto* worldTrans = reg.Get<Components::WorldTransformComponent>(entities[i]);
        JPH::Mat44  worldMat   = (worldTrans != nullptr) ? worldTrans->world : JPH::Mat44::sIdentity();
        JPH::Vec3   pos        = worldMat * meshes[i].localCenter;

        bool visibleInMain   = cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius);
        bool visibleInShadow = cam.shadowFrustum.IsSphereVisible(pos, meshes[i].cullRadius);
        if (visibleInMain || visibleInShadow) {
            expectedVisible++;
        }
    }

    if (visible.size() > entities.size()) {
        ZHLN::Log("[Test Fail] Culling: Visible count {} exceeds total entity count {}", visible.size(), entities.size());
    }

    for (Entity e: visible) {
        if (!reg.IsAlive(e)) {
            ZHLN::Log("[Test Fail] Culling: Visible list contains dead entity {}", e.index);
        }
    }

    if (visible.size() != expectedVisible && CullingStats::EnableCulling) {
        ZHLN::Log("[Test Fail] Culling: Visible count {} does not match expected {}", visible.size(), expectedVisible);
    }
}
}} // namespace ZHLN::Tests

namespace ZHLN {

template <bool UsePhysicsTransforms>
void CullingSystem::Update(Engine& engine, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow) {
    ZHLN::ScopedTimer profTimer("Culling (ECS O(N))");
    auto&             cam = engine.GetCamera();
    auto&             reg = engine.GetRegistry();
    auto&             rc  = engine.GetRenderContext();

    auto entities       = reg.GetEntitiesWith<Components::MeshComponent>();
    auto cameraEntities = reg.GetEntitiesWith<Components::CameraComponent>();

    Components::CameraComponent* cComp = nullptr;
    if (!cameraEntities.empty()) {
        cComp = reg.Get<Components::CameraComponent>(cameraEntities[0]);
    }

    bool     isFullBright     = false;
    float    shadowWidth      = 80.0f;
    uint32_t shadowResolution = 2048;

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
            isFullBright = (pp->fullBright != 0);
        }
    }

    auto shadowEntities = reg.GetEntitiesWith<Components::ShadowSettingsComponent>();
    if (!shadowEntities.empty()) {
        auto* shadowSettings = reg.Get<Components::ShadowSettingsComponent>(shadowEntities[0]);
        shadowWidth          = shadowSettings->shadowWidth;
        shadowResolution     = shadowSettings->shadowResolution;
    }

    static bool s_WasFrozen = false;
    if (CullingStats::FreezeFrustum) {
        if (!s_WasFrozen) {
            if (cComp != nullptr) {
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
            s_WasFrozen = true;
        }
        if (cComp != nullptr) {
            cam.frustum.Update(cComp->frozenViewProj);
        }
    } else {
        if (cComp != nullptr) {
            cam.frustum.Update(cComp->unjitteredViewProj);
        }
        s_WasFrozen = false;
    }

    if (!isFullBright) {
        auto [sunDirection, sunIntensity] = LightingSystem::GetSunDirectionAndIntensity(reg);

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

    auto meshes = reg.GetRawArray<Components::MeshComponent>();

    CullingStats::TotalTriangles    = 0;
    CullingStats::RenderedTriangles = 0;

    if (!CullingStats::EnableCulling) {
        outVisible.assign(entities.begin(), entities.end());
        outVisibleShadow.assign(entities.begin(), entities.end());

        uint32_t tris = 0;
        for (size_t i = 0; i < entities.size(); ++i) {
            auto gpuMeshOpt = rc.GetGPUMesh(meshes[i].meshAsset);
            if (gpuMeshOpt.has_value()) {
                tris += (gpuMeshOpt->indexCount > 0) ? (gpuMeshOpt->indexCount / 3) : (gpuMeshOpt->vertexCount / 3);
            }
        }
        CullingStats::TotalTriangles    = tris;
        CullingStats::RenderedTriangles = tris;
        return;
    }

    outVisible.clear();
    outVisibleShadow.clear();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e        = entities[i];
        const auto& meshComp = meshes[i];

        if ((meshComp.flags & DrawFlags::Hidden) != DrawFlags::None) {
            continue;
        }

        auto     gpuMeshOpt = rc.GetGPUMesh(meshComp.meshAsset);
        uint32_t meshTris   = 0;
        if (gpuMeshOpt.has_value()) {
            meshTris = (gpuMeshOpt->indexCount > 0) ? (gpuMeshOpt->indexCount / 3) : (gpuMeshOpt->vertexCount / 3);
        }

        CullingStats::TotalTriangles += meshTris;

        const auto* worldTrans = reg.Get<Components::WorldTransformComponent>(e);
        JPH::Mat44  worldMat   = (worldTrans != nullptr) ? worldTrans->world : JPH::Mat44::sIdentity();

        JPH::Vec3 pos             = worldMat * meshComp.localCenter;
        float     scaleX          = worldMat.GetColumn3(0).Length();
        float     scaleY          = worldMat.GetColumn3(1).Length();
        float     scaleZ          = worldMat.GetColumn3(2).Length();
        float     currentMaxScale = std::max({scaleX, scaleY, scaleZ});

        if (cam.frustum.IsSphereVisible(pos, meshComp.cullRadius * currentMaxScale)) {
            outVisible.push_back(e);
            CullingStats::RenderedTriangles += meshTris;
        }

        if (!isFullBright && cam.shadowFrustum.IsSphereVisible(pos, meshComp.cullRadius)) {
            outVisibleShadow.push_back(e);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyCullingResults(reg, outVisible, cam);
    }
}

void CullingSystem::DrawDebugFrustum(Engine& engine) {
    if (!CullingStats::FreezeFrustum) {
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
} // namespace ZHLN

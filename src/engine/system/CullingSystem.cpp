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

    // The invariant must mirror the REAL culling predicate exactly:
    //   * Hidden meshes are skipped before the frustum tests.
    //   * `outVisible` holds MAIN-camera visibility only (the shadow list is
    //     separate), so OR-ing the shadow frustum in here over-counted every
    //     off-screen mesh that merely intersects the huge ortho shadow volume.
    //   * The system inflates the test radius by the world-matrix max axis
    //     scale; the old verifier compared against the unscaled local radius.
    size_t expectedVisible = 0;
    for (size_t i = 0; i < entities.size(); ++i) {
        if ((meshes[i].flags & DrawFlags::Hidden) != DrawFlags::None) {
            continue;
        }

        const auto* worldTrans = reg.Get<Components::WorldTransformComponent>(entities[i]);
        JPH::Mat44  worldMat   = (worldTrans != nullptr) ? worldTrans->world : JPH::Mat44::sIdentity();
        JPH::Vec3   pos        = worldMat * meshes[i].localCenter;

        float currentMaxScale = std::max({worldMat.GetColumn3(0).Length(), worldMat.GetColumn3(1).Length(), worldMat.GetColumn3(2).Length()});

        if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius * currentMaxScale)) {
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

namespace {

// ============================================================================
// 4-wide SIMD frustum culling.
//
// Frustum::IsSphereVisible replicates ONE sphere across the SIMD lanes and
// tests 4 planes per instruction, wasting 3 of 4 lanes per test. This helper
// transposes the problem instead: 4 spheres ride in the lanes and each
// instruction tests one plane against all 4 of them, cutting the plane-test
// instruction count 4x. The predicate is bit-for-bit the one IsSphereVisible
// implements: strict `<` against the plane distance, radius inflated by the
// 0.5 m anti-flicker margin, and sentinel planes 6/7 that can never reject.
// ============================================================================
struct BatchedFrustum {
    static constexpr uint32_t kPlaneCount = 8;

    // Plane p: dot(n_p, center) + d_p < -(r + 0.5)  =>  sphere p rejected.
    std::array<float, kPlaneCount> nx {};
    std::array<float, kPlaneCount> ny {};
    std::array<float, kPlaneCount> nz {};
    std::array<float, kPlaneCount> pw {};

    [[nodiscard]] static BatchedFrustum FromFrustum(const Frustum& frustum) noexcept {
        BatchedFrustum out;
        for (uint32_t p = 0; p < kPlaneCount; ++p) {
            const uint32_t block = (p < 4) ? 0 : 1;
            const uint32_t lane  = p & 3;
            out.nx[p]            = frustum.mX[block].GetComponent(lane);
            out.ny[p]            = frustum.mY[block].GetComponent(lane);
            out.nz[p]            = frustum.mZ[block].GetComponent(lane);
            out.pw[p]            = frustum.mW[block].GetComponent(lane);
        }
        return out;
    }

    /// Tests 4 spheres (SoA lanes). `outVisible[j]` mirrors
    /// Frustum::IsSphereVisible(centers[j], radii[j]).
    void Test4(const JPH::Vec4& centersX, const JPH::Vec4& centersY, const JPH::Vec4& centersZ, const JPH::Vec4& negInflatedRadii, bool* outVisible) const noexcept {
        // Track the largest per-lane violation of `dist >= negRadius` across
        // all planes; a lane is visible iff the violation never goes positive.
        JPH::Vec4 worstViolation = JPH::Vec4::sReplicate(-1.0f);

        for (uint32_t p = 0; p < kPlaneCount; ++p) {
            JPH::Vec4 dist = JPH::Vec4::sReplicate(nx[p]) * centersX + JPH::Vec4::sReplicate(ny[p]) * centersY + JPH::Vec4::sReplicate(nz[p]) * centersZ +
                             JPH::Vec4::sReplicate(pw[p]);
            worstViolation = JPH::Vec4::sMax(worstViolation, negInflatedRadii - dist);
        }

        for (uint32_t j = 0; j < 4; ++j) {
            outVisible[j] = worstViolation.GetComponent(j) <= 0.0f;
        }
    }
};

} // namespace


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
        if (auto* shadowSettings = reg.Get<Components::ShadowSettingsComponent>(shadowEntities[0])) {
            shadowWidth      = shadowSettings->shadowWidth;
            shadowResolution = shadowSettings->shadowResolution;
        }
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

    // SIMD batching: gather 4 entities at a time, run both frustum tests
    // 4-wide, then do the per-entity bookkeeping from the visibility bits.
    constexpr size_t     kBatch = 4;
    const BatchedFrustum mainPlanes   = BatchedFrustum::FromFrustum(cam.frustum);
    const BatchedFrustum shadowPlanes = BatchedFrustum::FromFrustum(cam.shadowFrustum);

    std::array<Entity, kBatch>  batchEntities {};
    std::array<JPH::Vec3, kBatch> batchCenters {};
    std::array<float, kBatch>   batchRadii {};       // shadow-frustum radius (unscaled, matches the original test)
    std::array<float, kBatch>   batchScaledRadii {}; // main-frustum radius (inflated by world max scale)
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

            const auto* worldTrans = reg.Get<Components::WorldTransformComponent>(e);
            JPH::Mat44  worldMat   = (worldTrans != nullptr) ? worldTrans->world : JPH::Mat44::sIdentity();

            batchEntities[n] = e;
            batchCenters[n]  = worldMat * meshComp.localCenter;
            batchRadii[n]    = meshComp.cullRadius;
            batchScaledRadii[n] =
                meshComp.cullRadius * std::max({worldMat.GetColumn3(0).Length(), worldMat.GetColumn3(1).Length(), worldMat.GetColumn3(2).Length()});
            batchTris[n]   = meshTris;
            batchHidden[n] = hidden;

            CullingStats::TotalTriangles += hidden ? 0u : meshTris;

            ++i;
            ++n;
        }

        // Lanes beyond `n` are padding: their results are never read.
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
                CullingStats::RenderedTriangles += batchTris[j];
            }

            if (!isFullBright && shadowVisible[j]) {
                outVisibleShadow.push_back(batchEntities[j]);
            }
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

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "GraphicsSettingsSync.hpp"
#include "LightingSystem.hpp"
#include "UIRenderSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <physics/PhysicsDebug.hpp>

namespace ZHLN {

namespace {

/// Nominal frame period packed into `FrameUniforms::camPos.w`, which doubles
/// as the only frame counter the shaders can see (see
/// `FrameIndexFromCamPosW` in resources/shaders/blue_noise.slang).
///
/// 1/64 s rather than 1/60: a power of two multiplies exactly in float32, so
/// the shader recovers the integer frame index bit for bit. The old 0.0166f
/// did not, and past a couple of thousand frames consecutive frames decoded to
/// the same index -- freezing every blue-noise dither driven from this slot.
/// The mask keeps the product exact past 2^24 frames (~3 days at 64 Hz) by
/// wrapping the clock instead of letting it lose its low bits.
constexpr float    kFrameTimeStep  = 0.015625f;
constexpr uint64_t kFrameClockMask = 0xFFFFFFull;

} // namespace

std::expected<void, Error> RenderSystem::Update(Engine& engine, float dt) {
    int        physicsDrawMode = 0;
    JPH::Mat44 shadowProjView  = JPH::Mat44::sIdentity();

    auto mainResult = RenderMain(engine, physicsDrawMode, shadowProjView, dt);
    if (!mainResult) {
        return mainResult;
    }

    RenderDebug(engine, physicsDrawMode);

    auto& rc      = engine.GetRenderContext();
    auto  end_res = rc.EndFrame();
    if (!end_res) {
        return std::unexpected(end_res.error());
    }

    return {};
}

std::expected<void, Error> RenderSystem::RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView, float dt) {
    auto&       rc              = engine.GetRenderContext();
    auto&       reg             = engine.GetRegistry();
    auto&       cam             = engine.GetCamera();
    const auto& visibleEntities = engine.GetVisibleEntities();

    JPH::Mat44 vp {};
    JPH::Mat44 unjitteredVp {};
    JPH::Mat44 prevUnjitteredVp {};

    auto cameraEntities = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (cameraEntities.empty()) {
        return std::unexpected(RenderFrameResult::Error);
    }

    // --- Single graphics-settings sync point --------------------------------
    // ECS components are the editing surface (ImGui / scripts / presets);
    // GraphicsSettings is the canonical model. One collect + delta-detected
    // apply per frame replaces the former scattered SetGISettings /
    // SetAAState / SetShadowResolution calls: anything that mutates the
    // components — including Lua scripts — now gets reactive GPU updates
    // (e.g. cascade shadow-target resizes) without calling the renderer.
    const GraphicsSettings gfx = SyncGraphicsSettings(engine);

    auto begin_res = rc.BeginFrame();
    if (!begin_res) {
        return std::unexpected(begin_res.error());
    }
    UIRenderSystem::Update(engine);
    Entity cameraEntity = cameraEntities[0];

    if (auto* cComp = reg.Get<Components::CameraComponent>(cameraEntity)) {
        vp               = cComp->viewProj;
        unjitteredVp     = cComp->unjitteredViewProj;
        prevUnjitteredVp = cComp->prevUnjitteredViewProj;
    } else {
        return std::unexpected(RenderFrameResult::Error);
    }

    outPhysicsDrawMode = 0;
    if (auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>(); !settingsEntities.empty()) {
        if (auto* dbg = reg.Get<Components::DebugSettingsComponent>(settingsEntities[0])) {
            outPhysicsDrawMode = dbg->physicsDrawMode;
        }
    }

    auto [sunDirection, sunIntensity] = LightingSystem::GetSunDirectionAndIntensity(reg);

    const float    shadowWidth      = gfx.shadows.width;
    const uint32_t shadowResolution = gfx.shadows.resolution;

    float textelSize = shadowWidth / static_cast<float>(shadowResolution);

    JPH::Vec3 shadowCenter = cam.position;
    shadowCenter.SetX(std::round(shadowCenter.GetX() / textelSize) * textelSize);
    shadowCenter.SetY(std::round(shadowCenter.GetY() / textelSize) * textelSize);
    shadowCenter.SetZ(std::round(shadowCenter.GetZ() / textelSize) * textelSize);

    JPH::Vec3  lightPos  = shadowCenter + sunDirection * Shadows::FarOffset;
    JPH::Mat44 lightView = Math::CreateLookAt(lightPos, shadowCenter, JPH::Vec3::sAxisY());

    float      halfWidth = shadowWidth * 0.5f;
    JPH::Mat44 lightProj = Math::CreateOrtho(-halfWidth, halfWidth, -halfWidth, halfWidth, Shadows::NearClip, Shadows::FarDepth);
    outShadowProjView    = lightProj * lightView;

    cam.shadowFrustum.Update(outShadowProjView);

    const AAState& aaState = gfx.antiAliasing;

    FrameUniforms uniforms {};
    uniforms.viewProj               = vp;
    uniforms.unjitteredViewProj     = unjitteredVp;
    uniforms.prevUnjitteredViewProj = prevUnjitteredVp;
    uniforms.invViewProj            = unjitteredVp.Inversed();
    std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
    uniforms.camPos[3]       = static_cast<float>(engine.GetCurrentFrame() & kFrameClockMask) * kFrameTimeStep;
    JPH::Vec3 shaderLightDir = sunDirection;
    std::memcpy(&uniforms.lightDir[0], &shaderLightDir, sizeof(float) * 3);
    uniforms.lightDir[3]      = sunIntensity;
    uniforms.lightCount       = static_cast<uint32_t>(reg.GetEntitiesWith<Components::LightComponent>().size());
    uniforms.probeMin         = JPH::Vec4(
        gfx.environment.probeMin[0], gfx.environment.probeMin[1], gfx.environment.probeMin[2], gfx.environment.useLocalProbe ? 1.0f : 0.0f
    );
    uniforms.probeMax         = JPH::Vec4(gfx.environment.probeMax[0], gfx.environment.probeMax[1], gfx.environment.probeMax[2], 0.0f);
    uniforms.probePos         = JPH::Vec4(gfx.environment.probePos[0], gfx.environment.probePos[1], gfx.environment.probePos[2], 0.0f);
    uniforms.jitterParams     = JPH::Vec4(aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY);
    uniforms.enableRTR        = gfx.post.enableRTR;
    uniforms.fullBright       = gfx.environment.fullBright;
    uniforms.shadowWidth      = gfx.shadows.width;
    uniforms.shadowResolution = gfx.shadows.resolution;
    uniforms.sunSize          = gfx.shadows.sunSize;
    uniforms.ambientExposure  = gfx.environment.ambientExposure;
    uniforms.skyZenith        = JPH::Vec4(
        gfx.environment.skyZenith[0], gfx.environment.skyZenith[1], gfx.environment.skyZenith[2], gfx.environment.skyZenith[3]
    );
    uniforms.skyHorizon = JPH::Vec4(
        gfx.environment.skyHorizon[0], gfx.environment.skyHorizon[1], gfx.environment.skyHorizon[2], gfx.environment.skyHorizon[3]
    );
    uniforms.skyGround = JPH::Vec4(gfx.environment.skyGround[0], gfx.environment.skyGround[1], gfx.environment.skyGround[2], gfx.environment.skyGround[3]);

    rc.SetFrameData(cam, uniforms, outShadowProjView, dt);
    rc.SetMatrices(vp, unjitteredVp);

    const auto& mainVisible   = engine.GetVisibleEntities();
    const auto& shadowVisible = engine.GetVisibleShadowEntities();

    auto IsInList = [](const JPH::Array<Entity>& list, Entity e) -> bool { return std::ranges::find(list, e) != list.end(); };

    if (outPhysicsDrawMode == 0) {
        for (Entity e: reg.GetEntitiesWith<Components::MeshComponent>()) {
            bool inMain   = IsInList(mainVisible, e);
            bool inShadow = IsInList(shadowVisible, e);

            if (inMain || inShadow) {
                auto* meshComp = reg.Get<Components::MeshComponent>(e);
                if (meshComp == nullptr) {
                    continue;
                }

                auto gpuMeshOpt = rc.GetGPUMesh(meshComp->meshAsset);
                auto gpuMatOpt  = rc.GetGPUMaterial(meshComp->materialAsset);

                if (!gpuMeshOpt.has_value() || !gpuMatOpt.has_value()) {
                    continue;
                }

                Mesh     gpuMesh = *gpuMeshOpt;
                Material gpuMat  = *gpuMatOpt;

                auto* skelMesh   = reg.Get<Components::SkeletalMeshComponent>(e);
                auto* morphComp  = reg.Get<Components::MorphTargetComponent>(e);
                auto* worldTrans = reg.Get<Components::WorldTransformComponent>(e);

                JPH::Mat44 worldMat = (worldTrans != nullptr) ? worldTrans->world : JPH::Mat44::sIdentity();
                JPH::Mat44 prevMat  = (worldTrans != nullptr) ? worldTrans->previous : worldMat;

                bool     isSkinned   = (skelMesh != nullptr);
                uint32_t jointOffset = isSkinned ? skelMesh->jointOffset : 0;

                uint32_t     morphOffset      = (morphComp != nullptr) ? morphComp->offset : 0;
                uint32_t     activeMorphCount = (morphComp != nullptr) ? morphComp->activeCount : 0;
                const float* morphWeights     = (morphComp != nullptr) ? morphComp->weights.data() : nullptr;

                BufferHandle scratchVbo = BufferHandle::Invalid;
                if (isSkinned) {
                    scratchVbo = rc.GetOrCreateSkinnedScratchBuffer(e.Pack(), gpuMesh.vertexCount);
                }

                DrawFlags drawFlags = meshComp->flags;
                if (inMain) {
                    drawFlags |= DrawFlags::VisibleInMain;
                }
                if (inShadow) {
                    drawFlags |= DrawFlags::VisibleInShadow;
                }

                float roughness = -1.0f;
                float metallic  = -1.0f;
                if (auto* pbr = reg.Get<Components::PBRComponent>(e)) {
                    roughness = pbr->roughness;
                    metallic  = pbr->metallic;
                }

                if (auto* csg = reg.Get<Components::CSGComponent>(e)) {
                    CSGDrawParams csgParams;
                    csgParams.eyeParams = {
                        .transform           = worldMat,
                        .prevTransform       = prevMat,
                        .cullRadius          = meshComp->cullRadius,
                        .localCenter         = {meshComp->localCenter.GetX(), meshComp->localCenter.GetY(), meshComp->localCenter.GetZ()},
                        .jointOffset         = jointOffset,
                        .morphOffset         = morphOffset,
                        .activeMorphCount    = activeMorphCount,
                        .morphWeights        = morphWeights,
                        .flags               = drawFlags,
                        .skinnedVertexBuffer = scratchVbo,
                        .roughness           = roughness,
                        .metallic            = metallic
                    };

                    for (const auto& mod: csg->modifiers) {
                        if (reg.IsAlive(mod.operandEntity)) {
                            if (auto* cutMesh = reg.Get<Components::MeshComponent>(mod.operandEntity)) {
                                auto cutGpuMeshOpt = rc.GetGPUMesh(cutMesh->meshAsset);
                                auto cutGpuMatOpt  = rc.GetGPUMaterial(cutMesh->materialAsset);
                                if (cutGpuMeshOpt && cutGpuMatOpt) {
                                    auto*      cutSkelMesh   = reg.Get<Components::SkeletalMeshComponent>(mod.operandEntity);
                                    auto*      cutWorldTrans = reg.Get<Components::WorldTransformComponent>(mod.operandEntity);
                                    JPH::Mat44 cutWorldMat   = (cutWorldTrans != nullptr) ? cutWorldTrans->world : JPH::Mat44::sIdentity();
                                    JPH::Mat44 cutPrevMat    = (cutWorldTrans != nullptr) ? cutWorldTrans->previous : cutWorldMat;

                                    BufferHandle cutScratchVbo = BufferHandle::Invalid;
                                    if (cutSkelMesh != nullptr) {
                                        cutScratchVbo = rc.GetOrCreateSkinnedScratchBuffer(mod.operandEntity.Pack(), cutGpuMeshOpt->vertexCount);
                                    }

                                    csgParams.cutters.push_back(
                                        {.mesh                = *cutGpuMeshOpt,
                                         .material            = *cutGpuMatOpt,
                                         .transform           = cutWorldMat,
                                         .prevTransform       = cutPrevMat,
                                         .cullRadius          = cutMesh->cullRadius,
                                         .operation           = mod.operation,
                                         .jointOffset         = (cutSkelMesh != nullptr) ? cutSkelMesh->jointOffset : 0,
                                         .skinnedVertexBuffer = cutScratchVbo,
                                         .flags               = cutMesh->flags}
                                    );
                                }
                            }
                        }
                    }

                    if (!csgParams.cutters.empty()) {
                        rc.DrawCSG(gpuMat, gpuMesh, csgParams);
                        continue;
                    }
                }

                rc.Draw(
                    gpuMat, gpuMesh,
                    {.transform           = worldMat,
                     .prevTransform       = prevMat,
                     .cullRadius          = meshComp->cullRadius,
                     .localCenter         = {meshComp->localCenter.GetX(), meshComp->localCenter.GetY(), meshComp->localCenter.GetZ()},
                     .jointOffset         = jointOffset,
                     .morphOffset         = morphOffset,
                     .activeMorphCount    = activeMorphCount,
                     .morphWeights        = morphWeights,
                     .flags               = drawFlags,
                     .skinnedVertexBuffer = scratchVbo,
                     .roughness           = roughness,
                     .metallic            = metallic}
                );
            }
        }
    }

    CullingStats::TotalObjects  = reg.GetEntitiesWith<Components::MeshComponent>().size();
    CullingStats::CulledObjects = CullingStats::TotalObjects - visibleEntities.size();

    return {};
}

void RenderSystem::RenderDebug(Engine& engine, int physicsDrawMode) {
    auto& rc = engine.GetRenderContext();

    engine.GetCullingSystem().DrawDebugFrustum(engine);

    if (physicsDrawMode > 0) {
        ZHLN::ScopedTimer profTimer("Physics Debug Extract & Upload");

        static Material debugLineMat  = {.pipeline = PipelineHandle::Invalid};
        static Material debugSolidMat = {.pipeline = PipelineHandle::Invalid};

        static RenderContext* s_LastContext = nullptr;
        if (&rc != s_LastContext) {
            debugLineMat.pipeline  = PipelineHandle::Invalid;
            debugSolidMat.pipeline = PipelineHandle::Invalid;
            s_LastContext          = &rc;
        }

        if (debugLineMat.pipeline == PipelineHandle::Invalid) {
            auto debugLineMat_res = rc.CreateDebugLineMaterial();
            if (!debugLineMat_res) {
                ZHLN::Panic("Failed to compile debug line material: {}", debugLineMat_res.error().Message());
            }
            debugLineMat           = debugLineMat_res.value();
            debugLineMat.albedoMap = TextureHandle(1);

            auto debugSolidMat_res = rc.CreateDebugSolidMaterial();
            if (!debugSolidMat_res) {
                ZHLN::Panic("Failed to compile debug solid material: {}", debugSolidMat_res.error().Message());
            }
            debugSolidMat           = debugSolidMat_res.value();
            debugSolidMat.albedoMap = TextureHandle(1);
        }

        bool isWireframe = (physicsDrawMode == 1);
        auto debugData   = engine.GetPhysicsContext().GetDebugDrawData(true, true, isWireframe);

        std::vector<VertexPosition>   debugPos;
        std::vector<VertexAttributes> debugAttr;

        if (isWireframe && debugData.lineCount > 0) {
            auto UnpackColorVec4 = [](uint32_t packed) {
                float r = static_cast<float>(packed & 0xFF) / 255.0f;
                float g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
                float b = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
                float a = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
                return JPH::Vec4(r, g, b, a);
            };

            for (size_t i = 0; i + 1 < debugData.lineCount; i += 2) {
                const auto& v0 = debugData.lines[i];
                const auto& v1 = debugData.lines[i + 1];
                rc.DrawLine(JPH::Vec3(v0.x, v0.y, v0.z), JPH::Vec3(v1.x, v1.y, v1.z), UnpackColorVec4(v0.color), UnpackColorVec4(v1.color));
            }
        } else if (!isWireframe && debugData.triangleCount > 0) {
            debugPos.reserve(debugData.triangleCount);
            debugAttr.reserve(debugData.triangleCount);
            for (size_t i = 0; i < debugData.triangleCount; ++i) {
                const auto& jv = debugData.triangles[i];
                debugPos.push_back({.position = {jv.x, jv.y, jv.z}});
                debugAttr.push_back(
                    {.normal  = Math::PackNormal(0.0f, 1.0f, 0.0f),
                     .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                     .uv      = Math::PackUV(0.0f, 0.0f),
                     .color   = {.data = jv.color}}
                );
            }
        }

        if (!debugPos.empty()) {
            rc.UploadDebugVertices(
                debugPos.data(), debugPos.size() * sizeof(VertexPosition), debugAttr.data(), debugAttr.size() * sizeof(VertexAttributes),
                static_cast<uint32_t>(debugPos.size())
            );

            Mesh debugMesh = {
                .posBuffer   = rc.GetDebugMeshBuffer(),
                .attrBuffer  = rc.GetDebugMeshBuffer(),
                .skinBuffer  = BufferHandle::Invalid,
                .indexBuffer = BufferHandle::Invalid,
                .vertexCount = static_cast<uint32_t>(debugPos.size()),
                .indexCount  = 0
            };

            rc.Draw(
                isWireframe ? debugLineMat : debugSolidMat, debugMesh,
                {.transform = JPH::Mat44::sIdentity(), .prevTransform = JPH::Mat44::sIdentity(), .cullRadius = 10000.0f}
            );
        }
    }
}

} // namespace ZHLN

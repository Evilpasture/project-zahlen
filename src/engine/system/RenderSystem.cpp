// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "GraphicsSettingsSync.hpp"
#include "LightingSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ZHLN {

// Frame-composition errors
// The engine's own frame failures, as opposed to anything the renderer reports:
// this system can be told to draw a frame that has no main camera to draw it
// with, which no Vulkan call knows anything about.

enum class RenderSystemError : uint8_t {
    NoMainCamera ZHLN_ANNOTATION(ZHLN::Description<"The frame has no main camera entity to render the scene from"> {}) = 1,
};

namespace {

// Nominal frame period packed into `FrameUniforms::camPos.w`, which doubles
// as the only frame counter the shaders can see (see
// `FrameIndexFromCamPosW` in resources/shaders/blue_noise.slang).
//
// 1/64 s rather than 1/60: a power of two multiplies exactly in float32, so
// the shader recovers the integer frame index bit for bit. The old 0.0166f
// did not, and past a couple of thousand frames consecutive frames decoded to
// the same index -- freezing every blue-noise dither driven from this slot.
// The mask keeps the product exact past 2^24 frames (~3 days at 64 Hz) by
// wrapping the clock instead of letting it lose its low bits.
constexpr float    kFrameTimeStep  = 0.015625f;
constexpr uint64_t kFrameClockMask = 0xFFFFFFull;

void SubmitVisibleMeshes(Engine& engine, const JPH::Array<Entity>& mainVisible, const JPH::Array<Entity>& shadowVisible) {
    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();

    auto IsInList = [](const JPH::Array<Entity>& list, Entity e) -> bool { return std::ranges::find(list, e) != list.end(); };

    for (Entity e: reg.GetEntitiesWith<Components::MeshComponent>()) {
        bool inMain   = IsInList(mainVisible, e);
        bool inShadow = IsInList(shadowVisible, e);

        if (!inMain && !inShadow) {
            continue;
        }

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

[[nodiscard]] auto MakeViewportCamera(Engine& engine, Entity cameraEnt) -> Camera {
    Camera extra = engine.GetCamera();
    auto&  reg   = engine.GetRegistry();
    if (cameraEnt == Entity::Null() || !reg.IsAlive(cameraEnt)) {
        return extra;
    }
    if (auto* tc = reg.Get<Components::TargetCameraComponent>(cameraEnt); tc != nullptr) {
        extra.yaw   = tc->yaw;
        extra.pitch = tc->pitch;
        extra.fov   = tc->fov;
    }
    if (auto* world = reg.Get<Components::WorldTransformComponent>(cameraEnt); world != nullptr) {
        extra.position = world->world.GetTranslation();
    }
    return extra;
}

// Builds the optics of one camera entity into a SceneView for `target`.
SceneView MakeViewFor(Engine& engine, Entity cameraEnt, const RenderAttachment& target, const ViewportRect& viewport) {
    auto* cComp = engine.GetRegistry().Get<Components::CameraComponent>(cameraEnt);

    // One camera per view. An entity that owns a camera component is rendered by
    // the camera that component's matrices were built from: CameraSystem
    // projects the engine camera, so that is the camera this view describes and
    // the plain pair below is the unjittered partner of the matrix the frame is
    // actually rasterized with. The rasterization matrix is the component's own
    // viewProj, which carries the TAA subpixel jitter (GetJitteredProjectionMatrix)
    // whenever the camera's AA mode is TAA; taa.slang compensates for exactly
    // that jitter through frame.jitterParams.
    //
    // Deriving that pair from any other camera -- the entity's TargetCamera
    // overrides, say -- would put the depth buffer in one frustum and the cluster
    // cell the lighting pass picks in another: correct geometry, correct depth,
    // correct cluster bounds, and a cell lookup that misses. An entity without a
    // camera component has no component pair to partner, so its view is built
    // from its own optics alone and the two halves are the same pair by
    // construction.
    Camera           cam    = cComp != nullptr ? engine.GetCamera() : MakeViewportCamera(engine, cameraEnt);
    const float      aspect = viewport.height > 0 ? static_cast<float>(viewport.width) / static_cast<float>(viewport.height) : engine.GetRenderContext().GetViewportAspect();
    const JPH::Mat44 view   = cam.GetViewMatrix();
    const JPH::Mat44 proj   = cam.GetProjectionMatrix(aspect);
    const JPH::Mat44 viewProj = cComp != nullptr ? cComp->viewProj : proj * view;

    cam.frustum.Update(viewProj);

    return SceneView {
        .viewMatrix        = view,
        .projMatrix        = proj,
        .viewProjMatrix    = viewProj,
        .invViewProjMatrix = viewProj.Inversed(),
        .worldPosition     = cam.position,
        .viewport          = viewport,
        .target            = target,
        .frustum           = cam.frustum,
        .visibilityMask    = ~0ULL,
        .frameIndex        = static_cast<uint32_t>(engine.GetCurrentFrame()),
        .time              = static_cast<float>(engine.GetCurrentFrame() & kFrameClockMask) * kFrameTimeStep,
    };
}

} // namespace

std::expected<void, ErrorCode> RenderSystem::Update(Engine& engine, float dt) {
    int        physicsDrawMode = 0;
    JPH::Mat44 shadowProjView  = JPH::Mat44::sIdentity();

    auto mainResult = RenderMain(engine, physicsDrawMode, shadowProjView, dt);
    if (!mainResult) {
        return std::unexpected(mainResult.error());
    }
    if (mainResult->has_value()) {
        // FrameSkipped: there was nothing to draw into this frame, so there is
        // nothing to end either -- no frame was begun, and the next tick tries
        // again. Not a failure, and not something a caller of this system has to
        // hear about.
        return {};
    }

    RenderDebug(engine, physicsDrawMode);

    // The frame closes explicitly here: BeginFrame/EndFrame own synchronization
    // and presentation, and every draw was dispatched by name above. A 2D-only
    // client calls RenderUI instead and never pays for any of this.
    auto& rc      = engine.GetRenderContext();
    auto  end_res = rc.EndFrame();
    if (!end_res) {
        return std::unexpected(end_res.error());
    }
    // end_res->has_value() would be PresentSuboptimal: the frame was drawn, one
    // of its presents did not go through as asked, and the renderer has already
    // rebuilt the swapchain for it. Nothing for this system to do about it, and
    // nothing to report as a failure.

    return {};
}

FrameOutcome<FrameSkipped> RenderSystem::RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView, float dt) {
    auto&       rc              = engine.GetRenderContext();
    auto&       reg             = engine.GetRegistry();
    auto&       cam             = engine.GetCamera();
    const auto& visibleEntities = engine.GetVisibleEntities();

    JPH::Mat44 vp {};
    JPH::Mat44 unjitteredVp {};
    JPH::Mat44 prevUnjitteredVp {};

    auto cameraEntities = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (cameraEntities.empty()) {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }

    // --- Single graphics-settings sync point
    // ECS components are the editing surface (GUI / scripts / presets);
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
    if (begin_res->has_value()) {
        // FrameSkipped: nothing was begun (there was nothing to draw into this
        // frame), so nothing below can draw. Nothing is wrong -- the frame is
        // simply not this tick's.
        return FrameSkipped {};
    }
    Entity cameraEntity = cameraEntities[0];

    if (auto* cComp = reg.Get<Components::CameraComponent>(cameraEntity)) {
        vp               = cComp->viewProj;
        unjitteredVp     = cComp->unjitteredViewProj;
        prevUnjitteredVp = cComp->prevUnjitteredViewProj;
    } else {
        return std::unexpected(RenderSystemError::NoMainCamera);
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
    uniforms.lightDir[3] = sunIntensity;
    // lightCount is deliberately not set here: the renderer stamps it from the
    // light list SetLights actually packed (see SetFrameData). An entity count
    // taken here is a second opinion about the same array, and the two only
    // agree by luck.
    uniforms.probeMin =
        JPH::Vec4(gfx.environment.probeMin[0], gfx.environment.probeMin[1], gfx.environment.probeMin[2], gfx.environment.useLocalProbe ? 1.0f : 0.0f);
    uniforms.probeMax         = JPH::Vec4(gfx.environment.probeMax[0], gfx.environment.probeMax[1], gfx.environment.probeMax[2], 0.0f);
    uniforms.probePos         = JPH::Vec4(gfx.environment.probePos[0], gfx.environment.probePos[1], gfx.environment.probePos[2], 0.0f);
    uniforms.jitterParams     = JPH::Vec4(aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY);
    uniforms.enableRTR        = gfx.post.enableRTR;
    uniforms.fullBright       = gfx.environment.fullBright;
    uniforms.shadowWidth      = gfx.shadows.width;
    uniforms.shadowResolution = gfx.shadows.resolution;
    uniforms.sunSize          = gfx.shadows.sunSize;
    uniforms.ambientExposure  = gfx.environment.ambientExposure;
    uniforms.skyZenith  = JPH::Vec4(gfx.environment.skyZenith[0], gfx.environment.skyZenith[1], gfx.environment.skyZenith[2], gfx.environment.skyZenith[3]);
    uniforms.skyHorizon = JPH::Vec4(gfx.environment.skyHorizon[0], gfx.environment.skyHorizon[1], gfx.environment.skyHorizon[2], gfx.environment.skyHorizon[3]);
    uniforms.skyGround  = JPH::Vec4(gfx.environment.skyGround[0], gfx.environment.skyGround[1], gfx.environment.skyGround[2], gfx.environment.skyGround[3]);

    rc.SetFrameData(cam, uniforms, outShadowProjView, dt);
    rc.SetMatrices(vp, unjitteredVp);

    if (outPhysicsDrawMode == 0) {
        SubmitVisibleMeshes(engine, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
    }

    // Compute simulations (cluster culling, volumetric fog, particle updates)
    // run on the async compute queue ahead of the scene graph; the graphics
    // submit waits on their timeline before the passes sample what they wrote.
    rc.DispatchCompute(dt);

    // One view, one destination. The attachment is acquired before the scene is
    // recorded: acquiring is what takes the window's image and opens the
    // destination's command buffer for this frame, and the caller -- not the
    // renderer -- decides what gets drawn into it.
    const ViewportRect viewport = rc.GetViewport();
    // The kernel resolves which target this frame draws into; the renderer's
    // low-level verb only wants the seam object, and this is the last place it
    // is named in the frame path.
    const auto target = engine.AcquireTarget();
    if (!target) {
        // The window could not become a destination this frame. It is said here
        // because this is the call that asked, and once because it is the call
        // that asks every frame: the renderer hands back the reason, and what to
        // do with it is the frame's decision, not the acquiring call's.
        ZHLN::Log("[Render] Window attachment refused: {}", target.error());
    }
    // Nothing acquired is not a failure: a swapchain image that was not handed
    // out leaves the frame with nothing to draw into, and the passes skip what
    // they cannot draw into.
    const RenderAttachment attachment = target.value_or(std::nullopt).value_or(RenderAttachment {});
    const SceneView     sceneView = MakeViewFor(engine, cameraEntity, attachment, viewport);
    rc.RenderScene(sceneView, gfx);

    // 2D UI the UI phase built (HUD, editor chrome) is composed over the
    // finished frame, into the same attachment. The payload carries its own
    // geometry, so this costs one dynamic pass and never a 3D pass.
    if (const UIDrawData uiData = engine.GetPendingUIData(); !uiData.Empty()) {
        rc.RenderUI(
            UIView {
                .viewport   = viewport,
                .target     = attachment,
                .frameIndex = static_cast<uint32_t>(engine.GetCurrentFrame()),
            },
            uiData
        );
        engine.SetPendingUIData(UIDrawData {});
    }

    auto& cstats = engine.GetCullingSystem().Stats();
    cstats.TotalObjects  = reg.GetEntitiesWith<Components::MeshComponent>().size();
    cstats.CulledObjects = cstats.TotalObjects - visibleEntities.size();

    return {};
}

void RenderSystem::RenderDebug(Engine& engine, int physicsDrawMode) {
    auto& rc = engine.GetRenderContext();

    engine.GetCullingSystem().DrawDebugFrustum(engine);

    if (physicsDrawMode > 0) {
        ZHLN::ScopedTimer profTimer("Physics Debug Extract & Upload");

        // The materials live on the render context's Impl (see
        // RenderContext::GetDebug*Material): a destroyed-and-reallocated
        // context, or a second coexisting one, gets its own handles instead of
        // the former function-local statics keying off raw pointer equality.
        auto debugLineMat_res = rc.GetDebugLineMaterial();
        if (!debugLineMat_res) {
            ZHLN::Panic("Failed to compile debug line material: {}", debugLineMat_res.error());
        }
        auto debugSolidMat_res = rc.GetDebugSolidMaterial();
        if (!debugSolidMat_res) {
            ZHLN::Panic("Failed to compile debug solid material: {}", debugSolidMat_res.error());
        }
        const Material debugLineMat  = *debugLineMat_res;
        const Material debugSolidMat = *debugSolidMat_res;

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

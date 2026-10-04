// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderSystem.hpp"
#include "CullingSystem.hpp"
#include "GraphicsSettingsSync.hpp"
#include "LightingSystem.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/World.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/PoseUploads.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace ZHLN {


enum class RenderSystemError : uint8_t {
    NoMainCamera ZHLN_ANNOTATION(ZHLN::Description<"The frame has no main camera entity to render the scene from"> {}) = 1,
    EnvironmentImageUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The scene's environment image was not supplied to AssetManager"> {}),
};

namespace {

[[nodiscard]] auto EnsureSkinnedScratch(RenderContext& rc, Components::SkeletalMeshComponent& skeleton, uint32_t vertexCount) -> BufferHandle {
    if (skeleton.skinnedScratch != BufferHandle::Invalid && skeleton.scratchVertexCount != vertexCount) {
        rc.DestroyBuffer(skeleton.skinnedScratch);
        skeleton.skinnedScratch     = BufferHandle::Invalid;
        skeleton.scratchVertexCount = 0;
    }
    if (skeleton.skinnedScratch == BufferHandle::Invalid && vertexCount != 0) {
        skeleton.skinnedScratch = rc.CreateSkinnedScratchBuffer(vertexCount);
        if (skeleton.skinnedScratch != BufferHandle::Invalid) {
            skeleton.scratchVertexCount = vertexCount;
        }
    }
    return skeleton.skinnedScratch;
}

[[nodiscard]] auto HasSceneSun(const ECS::Registry& reg) noexcept -> bool {
    for (const Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
        if (const auto light = reg.Get<Components::LightComponent>(e); light && light->type == LightType::Sun) {
            return true;
        }
    }
    return !reg.GetEntitiesWith<Components::SunTagComponent>().empty();
}

[[nodiscard]] auto SyncEnvironmentMap(Engine& engine) -> std::expected<void, ErrorCode> {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();
    const Entity ent = reg.SingletonEntity<Components::EnvironmentMapComponent>();
    if (ent == Entity::Null()) {
        return rc.SetEnvironmentRadiance({});
    }
    const auto env = reg.Get<Components::EnvironmentMapComponent>(ent);
    if (!env || env->source.empty()) {
        return rc.SetEnvironmentRadiance({});
    }
    const auto pixels = engine.GetAssetManager().FindEnvironmentImage(std::string_view(env->source));
    if (!pixels) {
        Log("[IBL] No prepared environment pixels registered for '{}'", std::string_view(env->source));
        return std::unexpected(RenderSystemError::EnvironmentImageUnavailable);
    }
    const std::span<const std::array<float, 3>> sh = pixels->sun
        ? std::span<const std::array<float, 3>> {pixels->sun->diffuseSH}
        : std::span<const std::array<float, 3>> {};
    return rc.SetEnvironmentRadiance({
        .rgba         = pixels->rgba,
        .lightingRgba = pixels->lightingRgba,
        .diffuseSH    = sh,
        .extent       = {.width = pixels->width, .height = pixels->height},
        .contentHash  = pixels->contentHash,
        .renderSkybox = env->renderSkybox != 0,
    });
}


constexpr float    kFrameTimeStep  = 0.015625f;
constexpr uint64_t kFrameClockMask = 0xFFFFFFull;

constexpr MaterialID kPhysicsDebugMaterialID = HashAssetID("builtin_physics_debug_solid_material");

[[nodiscard]] auto GetOrCreatePhysicsDebugMaterial(RenderContext& rc) -> std::optional<Material> {
    if (auto existing = rc.GetGPUMaterial(kPhysicsDebugMaterialID)) {
        return existing;
    }
    auto created = rc.CreateBasicMaterial(true, true);
    if (!created) {
        ZHLN::Log("[RenderSystem] Physics debug material creation failed: {}", created.error());
        return std::nullopt;
    }
    rc.RegisterGPUMaterial(kPhysicsDebugMaterialID, *created);
    return *created;
}

// GPU storage for one emitter: sized to maxParticles by the renderer's own
// stride, rebuilt if the capacity changed. The layout of a particle is the
// renderer's business; the engine only knows how many it asked for.
template <typename Emitter>
[[nodiscard]] auto EnsureParticleStorage(RenderContext& rc, Emitter& emitter, uint32_t stride) -> BufferHandle {
    if (emitter.gpuBuffer != BufferHandle::Invalid && emitter.bufferCapacity != emitter.maxParticles) {
        rc.DestroyBuffer(emitter.gpuBuffer);
        emitter.gpuBuffer      = BufferHandle::Invalid;
        emitter.bufferCapacity = 0;
    }
    if (emitter.gpuBuffer == BufferHandle::Invalid && emitter.maxParticles != 0) {
        emitter.gpuBuffer = rc.CreateStorageBuffer(static_cast<size_t>(emitter.maxParticles) * stride);
        if (emitter.gpuBuffer != BufferHandle::Invalid) {
            emitter.bufferCapacity = emitter.maxParticles;
        }
    }
    return emitter.gpuBuffer;
}

// Emitters are component state -- a description and a buffer handle -- so the
// submit step reads them here rather than a simulation system reaching into the
// renderer. The buffer handle lives on the component and is filled in here.
void SubmitParticleEmitters(Engine& engine) {
    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();
    const auto& cam = engine.GetCamera();

    for (auto& emitter: reg.GetRawArray<Components::ParticleEmitterComponent>()) {
        if (!emitter.active) {
            continue;
        }
        const BufferHandle buffer = EnsureParticleStorage(rc, emitter, rc.ParticleStride());
        // The component's params *are* the shader's params; the only thing the
        // system owns here is the camera attachment, so it is applied on the
        // renderer's copy rather than by hand-packing a second struct.
        ParticleEmitterParams desc = emitter.params;
        if (emitter.attachToCamera) {
            desc.spawnOrigin.x = cam.position.GetX();
            desc.spawnOrigin.y = cam.position.GetY();
            desc.spawnOrigin.z = cam.position.GetZ();
        }
        rc.SubmitParticleEmitter(buffer, emitter.maxParticles, desc);
    }

    for (auto& emitter: reg.GetRawArray<Components::MeshParticleEmitterComponent>()) {
        if (!emitter.active) {
            continue;
        }
        const BufferHandle buffer = EnsureParticleStorage(rc, emitter, rc.MeshParticleStride());
        rc.SubmitMeshParticleEmitter(buffer, emitter.maxParticles, emitter.params, emitter.meshAsset, emitter.materialAsset);
    }
}

// The palettes the simulation pushed this frame, applied in order.
void UploadPosePalettes(Engine& engine) {
    auto&                       rc      = engine.GetRenderContext();
    std::vector<PoseUpload>     uploads = engine.GetPoseUploads().Take();
    for (const auto& upload: uploads) {
        rc.UpdateJointMatrices(upload.jointOffset, upload.matrices);
    }
}

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

        auto meshComp = reg.Get<Components::MeshComponent>(e);
        if (!meshComp) {
            continue;
        }

        auto gpuMeshOpt = rc.GetGPUMesh(meshComp->meshAsset);
        if (!gpuMeshOpt.has_value()) {
            // Explicit cache clears discard lookups, not scene-owned buffers.
            // Rebind the owner's view instead of allocating another mesh.
            if (const auto owned = reg.Get<Components::OwnedMeshComponent>(e);
                owned && owned->meshAsset == meshComp->meshAsset && owned->mesh.posBuffer != BufferHandle::Invalid) {
                rc.RegisterGPUMesh(meshComp->meshAsset, owned->mesh);
                gpuMeshOpt = owned->mesh;
            }
        }
        auto gpuMatOpt = rc.GetGPUMaterial(meshComp->materialAsset);
        if (!gpuMeshOpt.has_value() || !gpuMatOpt.has_value()) {
            continue;
        }

        Mesh     gpuMesh = *gpuMeshOpt;
        Material gpuMat  = *gpuMatOpt;

        auto skelMesh   = reg.Get<Components::SkeletalMeshComponent>(e);
        auto morphComp  = reg.Get<Components::MorphTargetComponent>(e);
        auto worldTrans = reg.Get<Components::WorldTransformComponent>(e);

        JPH::Mat44 worldMat = worldTrans ? worldTrans->world : JPH::Mat44::sIdentity();
        JPH::Mat44 prevMat  = worldTrans ? worldTrans->previous : worldMat;

        bool     isSkinned   = skelMesh.has_value();
        uint32_t jointOffset = isSkinned ? skelMesh->jointOffset : 0;

        uint32_t                   morphOffset      = morphComp ? morphComp->offset : 0;
        uint32_t                   activeMorphCount = morphComp ? morphComp->activeCount : 0;
        const JPH::Float4          morphWeights     = morphComp ? morphComp->weights : JPH::Float4 {};

        BufferHandle scratchVbo = BufferHandle::Invalid;
        if (isSkinned) {
            scratchVbo = EnsureSkinnedScratch(rc, *skelMesh, gpuMesh.vertexCount);
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
        if (auto pbr = reg.Get<Components::PBRComponent>(e)) {
            roughness = pbr->roughness;
            metallic  = pbr->metallic;
        }

        if (auto csg = reg.Get<Components::CSGComponent>(e)) {
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
                    if (auto cutMesh = reg.Get<Components::MeshComponent>(mod.operandEntity)) {
                        auto cutGpuMeshOpt = rc.GetGPUMesh(cutMesh->meshAsset);
                        auto cutGpuMatOpt  = rc.GetGPUMaterial(cutMesh->materialAsset);
                        if (cutGpuMeshOpt && cutGpuMatOpt) {
                            auto       cutSkelMesh   = reg.Get<Components::SkeletalMeshComponent>(mod.operandEntity);
                            auto       cutWorldTrans = reg.Get<Components::WorldTransformComponent>(mod.operandEntity);
                            JPH::Mat44 cutWorldMat   = cutWorldTrans ? cutWorldTrans->world : JPH::Mat44::sIdentity();
                            JPH::Mat44 cutPrevMat    = cutWorldTrans ? cutWorldTrans->previous : cutWorldMat;

                            BufferHandle cutScratchVbo = BufferHandle::Invalid;
                            if (cutSkelMesh) {
                                cutScratchVbo = EnsureSkinnedScratch(rc, *cutSkelMesh, cutGpuMeshOpt->vertexCount);
                            }

                            csgParams.cutters.push_back(
                                {.mesh                = *cutGpuMeshOpt,
                                 .material            = *cutGpuMatOpt,
                                 .transform           = cutWorldMat,
                                 .prevTransform       = cutPrevMat,
                                 .cullRadius          = cutMesh->cullRadius,
                                 .operation           = mod.operation,
                                 .jointOffset         = cutSkelMesh ? cutSkelMesh->jointOffset : 0,
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
    if (auto world = reg.Get<Components::WorldTransformComponent>(cameraEnt)) {
        extra.position = world->world.GetTranslation();
    }
    return extra;
}

SceneView MakeViewFor(Engine& engine, Entity cameraEnt, const FrameTarget& target, const ViewportRect& viewport) {
    auto cComp = engine.GetRegistry().Get<Components::CameraComponent>(cameraEnt);

    // The view's camera is the entity's own component when it has one; a bare
    // transform entity gets a viewport camera built from the main camera's
    // optics at that entity's position.
    Camera           cam    = cComp ? cComp->camera : MakeViewportCamera(engine, cameraEnt);
    const float      aspect = viewport.height > 0 ? static_cast<float>(viewport.width) / static_cast<float>(viewport.height) : engine.GetRenderContext().GetViewportAspect();
    const JPH::Mat44 view   = cam.GetViewMatrix();
    const JPH::Mat44 proj   = cam.GetProjectionMatrix(aspect);
    const JPH::Mat44 viewProj = cComp ? cComp->viewProj : proj * view;

    // The view's planes belong to the view, not to the camera: handed to the
    // renderer with the rest of the SceneView.
    Frustum frustum {};
    frustum.Update(viewProj);

    return SceneView {
        .viewMatrix        = view,
        .projMatrix        = proj,
        .viewProjMatrix    = viewProj,
        .invViewProjMatrix = viewProj.Inversed(),
        .worldPosition     = cam.position,
        .viewport          = viewport,
        .target            = target,
        .frustum           = frustum,
        .visibilityMask    = ~0ULL,
        .frameIndex        = static_cast<uint32_t>(engine.GetCurrentFrame()),
        .time              = static_cast<float>(engine.GetCurrentFrame() & kFrameClockMask) * kFrameTimeStep,
    };
}

}

std::expected<void, ErrorCode> RenderSystem::Update(Engine& engine, float dt) {
    int        physicsDrawMode = 0;
    JPH::Mat44 shadowProjView  = JPH::Mat44::sIdentity();

    auto mainResult = RenderMain(engine, physicsDrawMode, shadowProjView, dt);
    if (!mainResult) {
        return std::unexpected(mainResult.error());
    }
    if (mainResult->has_value()) {
        return {};
    }

    RenderDebug(engine, physicsDrawMode);

    auto& rc      = engine.GetRenderContext();
    auto  end_res = rc.EndFrame();
    if (!end_res) {
        return std::unexpected(end_res.error());
    }

    return {};
}

FrameOutcome<FrameSkipped> RenderSystem::RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView, float dt) {
    auto&       rc              = engine.GetRenderContext();
    auto&       reg             = engine.GetRegistry();
    // A copy: the pose this frame renders with. Holding a reference into the
    // registry across the whole submit would dangle if that component's storage
    // ever moved.
    const Camera cam            = engine.GetCamera();
    const auto& visibleEntities = engine.GetVisibleEntities();

    JPH::Mat44 vp {};
    JPH::Mat44 unjitteredVp {};
    JPH::Mat44 prevUnjitteredVp {};

    auto cameraEntities = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (cameraEntities.empty()) {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }

    const GraphicsSettings gfx = SyncGraphicsSettings(engine);

    auto begin_res = rc.BeginFrame();
    if (!begin_res) {
        return std::unexpected(begin_res.error());
    }
    if (begin_res->has_value()) {
        return FrameSkipped {};
    }
    if (auto env = SyncEnvironmentMap(engine); !env) {
        (void)rc.EndFrame();
        return std::unexpected(env.error());
    }
    Entity cameraEntity = cameraEntities[0];

    if (auto cComp = reg.Get<Components::CameraComponent>(cameraEntity)) {
        vp               = cComp->viewProj;
        unjitteredVp     = cComp->unjitteredViewProj;
        prevUnjitteredVp = cComp->prevUnjitteredViewProj;
    } else {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }

    outPhysicsDrawMode = 0;
    if (auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>(); !settingsEntities.empty()) {
        if (auto dbg = reg.Get<Components::DebugSettingsComponent>(settingsEntities[0])) {
            outPhysicsDrawMode = dbg->physicsDrawMode;
        }
    }

    auto sun = LightingSystem::GetSun(LightingSystem::SunQuery {reg});
    if (const Entity envEnt = reg.SingletonEntity<Components::EnvironmentMapComponent>(); envEnt != Entity::Null() && !HasSceneSun(reg)) {
        if (const auto env = reg.Get<Components::EnvironmentMapComponent>(envEnt); env && !env->source.empty()) {
            sun.intensity = 0.0f;
        }
    }
    const JPH::Vec3 sunDirection = sun.direction;
    // The extracted emitter used to live in EvaluateSH and inherited the
    // environment exposure. Convert its irradiance/pi to directional radiance
    // (pi * irradiance), then apply that same exposure only to cooked suns.
    const float sunIntensity = sun.intensity * (sun.fromEnvironment ? gfx.environment.ambientExposure : 1.0f);
    const JPH::Vec3 sunRadiance = sun.color * sunIntensity;

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

    // The shadow planes are the culler's own: CullingSystem derives them from
    // the same sun and settings, and this matrix is what the passes consume.

    const AAState& aaState = gfx.antiAliasing;

    // The shader's own struct: the engine authors the view, the sun, the sky,
    // the probe and the jitter, and hands it over. The renderer fills what only
    // it knows (RenderContext::SetFrameData).
    const float frameClock = static_cast<float>(engine.GetCurrentFrame() & kFrameClockMask) * kFrameTimeStep;

    FrameUniforms viewData {};
    viewData.viewProj               = vp;
    viewData.unjitteredViewProj     = unjitteredVp;
    viewData.prevUnjitteredViewProj = prevUnjitteredVp;
    viewData.invViewProj            = unjitteredVp.Inversed();
    // The clock rides camPos.w and the sun's intensity rides lightDir.w because
    // those are the lanes the shader reads them from.
    viewData.camPos      = JPH::Float4 {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ(), frameClock};
    viewData.lightDir    = JPH::Float4 {sunDirection.GetX(), sunDirection.GetY(), sunDirection.GetZ(), sunIntensity};
    viewData.sunRadiance = JPH::Float4 {sunRadiance.GetX(), sunRadiance.GetY(), sunRadiance.GetZ(), 0.0f};
    viewData.probeMin =
        JPH::Float4 {gfx.environment.probeMin[0], gfx.environment.probeMin[1], gfx.environment.probeMin[2], gfx.environment.useLocalProbe ? 1.0f : 0.0f};
    viewData.probeMax     = JPH::Float4 {gfx.environment.probeMax[0], gfx.environment.probeMax[1], gfx.environment.probeMax[2], 0.0f};
    viewData.probePos     = JPH::Float4 {gfx.environment.probePos[0], gfx.environment.probePos[1], gfx.environment.probePos[2], 0.0f};
    viewData.jitterParams = JPH::Float4 {aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY};
    viewData.enableRTR        = gfx.post.enableRTR;
    viewData.fullBright       = gfx.environment.fullBright;
    viewData.shadowWidth      = gfx.shadows.width;
    viewData.shadowResolution = gfx.shadows.resolution;
    viewData.sunSize          = gfx.shadows.sunSize;
    viewData.ambientExposure  = gfx.environment.ambientExposure;
    viewData.skyZenith =
        JPH::Float4 {gfx.environment.skyZenith[0], gfx.environment.skyZenith[1], gfx.environment.skyZenith[2], gfx.environment.skyZenith[3]};
    viewData.skyHorizon =
        JPH::Float4 {gfx.environment.skyHorizon[0], gfx.environment.skyHorizon[1], gfx.environment.skyHorizon[2], gfx.environment.skyHorizon[3]};
    viewData.skyGround =
        JPH::Float4 {gfx.environment.skyGround[0], gfx.environment.skyGround[1], gfx.environment.skyGround[2], gfx.environment.skyGround[3]};

    rc.SetFrameData(cam, viewData, outShadowProjView, dt);
    rc.SetMatrices(vp, unjitteredVp);

    // Everything the simulation produced for the GPU goes in before the passes
    // read it: this frame's poses, then the emitters that consume them.
    UploadPosePalettes(engine);
    SubmitParticleEmitters(engine);

    if (outPhysicsDrawMode == 0) {
        SubmitVisibleMeshes(engine, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
    }

    if (auto sim_res = rc.DispatchSimulations(dt); !sim_res) {
        return std::unexpected(sim_res.error());
    }

    const ViewportRect viewport = rc.GetViewport();
    const auto target = engine.AcquireTarget();
    if (!target) {
        return std::unexpected(target.error());
    }
    if (!target->has_value()) {
        return {};
    }
    const FrameTarget attachment = **target;
    const SceneView sceneView = MakeViewFor(engine, cameraEntity, attachment, viewport);
    if (auto scene_res = rc.RenderScene(sceneView, gfx); !scene_res) {
        return std::unexpected(scene_res.error());
    }

    if (const UIDrawData uiData = engine.GetPendingUIData(); !uiData.Empty()) {
        if (auto ui_res = rc.RenderUI(
                UIView {
                    .viewport   = viewport,
                    .target     = attachment,
                    .frameIndex = static_cast<uint32_t>(engine.GetCurrentFrame()),
                },
                uiData
            );
            !ui_res) {
            return std::unexpected(ui_res.error());
        }
        engine.SetPendingUIData(UIDrawData {});
    }

    auto& cstats = engine.GetWorld().GetCullingStats();
    cstats.TotalObjects  = reg.GetEntitiesWith<Components::MeshComponent>().size();
    cstats.CulledObjects = cstats.TotalObjects - visibleEntities.size();

    return {};
}

void RenderSystem::RenderDebug(Engine& engine, int physicsDrawMode) {
    auto& rc = engine.GetRenderContext();

    CullingSystem::DrawDebugFrustum(engine);

    if (physicsDrawMode > 0) {
        ZHLN::ScopedTimer profTimer("Physics Debug Extract & Upload");

        bool isWireframe = (physicsDrawMode == 1);
        auto debugData   = engine.GetPhysicsContext().GetDebugDrawData(true, true, isWireframe);

        if (isWireframe) {
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
        } else if (debugData.triangleCount > 0) {
            auto debugMat = GetOrCreatePhysicsDebugMaterial(rc);
            if (!debugMat) {
                return;
            }

            std::vector<VertexPosition> debugPos;
            std::vector<VertexSurface>  debugSurface;
            debugPos.reserve(debugData.triangleCount);
            debugSurface.reserve(debugData.triangleCount);
            for (size_t i = 0; i < debugData.triangleCount; ++i) {
                const auto& jv = debugData.triangles[i];
                debugPos.push_back({.position = {jv.x, jv.y, jv.z}});
                debugSurface.push_back({.uv = Math::PackUV(0.0f, 0.0f), .color = {.data = jv.color}});
            }

            const uint32_t uploadedVertices = rc.UploadDebugVertices(std::span {debugPos}, std::span {debugSurface});

            Mesh debugMesh = {
                .posBuffer     = rc.GetDebugMeshBuffer(),
                .surfaceBuffer = rc.GetDebugMeshBuffer(),
                .skinBuffer    = BufferHandle::Invalid,
                .indexBuffer   = BufferHandle::Invalid,
                .vertexCount   = uploadedVertices,
                .indexCount    = 0
            };

            rc.Draw(
                *debugMat, debugMesh,
                {.transform = JPH::Mat44::sIdentity(), .prevTransform = JPH::Mat44::sIdentity(), .cullRadius = 10000.0f}
            );
        }
    }
}

}

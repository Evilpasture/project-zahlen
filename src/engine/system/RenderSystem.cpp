// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../EngineAccess.hpp"
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include "RenderSystem.hpp"
#include "CullingSystem.hpp"
#include "LightingSystem.hpp"
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Physics/Physics.hpp>
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

[[nodiscard]] auto EnsureSkinnedScratch(GraphicsDevice rc, Components::SkeletalMeshComponent& skeleton, uint32_t vertexCount) -> BufferID {
    if (skeleton.skinnedScratch != BufferID::Invalid && skeleton.scratchVertexCount != vertexCount) {
        rc.DestroyBuffer(skeleton.skinnedScratch);
        skeleton.skinnedScratch     = BufferID::Invalid;
        skeleton.scratchVertexCount = 0;
    }
    if (skeleton.skinnedScratch == BufferID::Invalid && vertexCount != 0) {
        skeleton.skinnedScratch = rc.CreateBuffer<BufferUsage::SkinScratch>(vertexCount);
        if (skeleton.skinnedScratch != BufferID::Invalid) {
            skeleton.scratchVertexCount = vertexCount;
        }
    }
    return skeleton.skinnedScratch;
}

using SceneSunQuery = ECS::Query<const Components::LightComponent, const Components::SunTagComponent>;

[[nodiscard]] auto HasSceneSun(SceneSunQuery query) noexcept -> bool {
    for (const Entity e: query.Entities<Components::LightComponent>()) {
        if (const auto light = query.Get<Components::LightComponent>(e); light && light->type == LightType::Sun) {
            return true;
        }
    }
    return !query.Entities<Components::SunTagComponent>().empty();
}

using EnvironmentQuery = ECS::Query<const Components::EnvironmentMapComponent>;

[[nodiscard]] auto SyncEnvironmentMap(EnvironmentQuery query, GraphicsDevice rc, AssetLibrary assets) -> std::expected<void, ErrorCode> {
    const auto env = query.GetSingleton<Components::EnvironmentMapComponent>();
    if (!env || env->source.empty()) {
        return rc.SetEnvironmentRadiance({});
    }
    const auto pixels = assets.FindEnvironmentImage(std::string_view(env->source));
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

// The simulation description stays in ECS; only a stable emitter identity crosses
// the API. Persistent particle buffers are private to the RenderContext.
using ParticleSubmitQuery = ECS::Query<const Components::CameraComponent, const Components::ParticleEmitterComponent, const Components::MeshParticleEmitterComponent>;

void SubmitParticleEmitters(ParticleSubmitQuery query, DebugDraw scene) {
    // The frame's camera, when the world has one: an attachment is a scene decision
    // made against the view, so a world without a camera has none to submit and the
    // render path reports NoMainCamera on its own.
    const auto camera = query.GetSingleton<Components::CameraComponent>();
    if (!camera) {
        return;
    }
    const auto& cam = camera->camera;

    for (const Entity entity: query.Entities<Components::ParticleEmitterComponent>()) {
        auto emitter = query.Get<Components::ParticleEmitterComponent>(entity);
        if (!emitter || !emitter->active) {
            continue;
        }
        // The component's description goes to the renderer as it is; the only thing the
        // system owns here is the camera attachment, which is a scene decision.
        ParticleEmitterDesc desc = emitter->params;
        if (emitter->attachToCamera) {
            desc.spawnOrigin = cam.position;
        }
        scene.SubmitParticles(
            {.emitterId = entity.Pack(), .maxParticles = emitter->maxParticles, .desc = desc, .texture = emitter->textureAsset, .additive = emitter->additive}
        );
    }

    for (const Entity entity: query.Entities<Components::MeshParticleEmitterComponent>()) {
        auto emitter = query.Get<Components::MeshParticleEmitterComponent>(entity);
        if (!emitter || !emitter->active) {
            continue;
        }
        scene.SubmitMeshParticles(
            {.emitterId    = entity.Pack(),
             .maxParticles = emitter->maxParticles,
             .desc         = emitter->params,
             .mesh         = emitter->meshAsset,
             .mat          = emitter->materialAsset}
        );
    }
}

// The palettes the simulation pushed this frame, applied in order.
void UploadPosePalettes(Engine& engine) {
    auto rc = engine.Graphics();
    ZHLN::EngineAccess::PoseUploadsOf(engine).Drain([&](const PoseUpload& upload) { rc.UpdateJointMatrices(upload.jointOffset, upload.matrices); });
}

using MeshSubmitQuery = ECS::Query<
    const Components::MeshComponent, const Components::OwnedMeshComponent, Components::SkeletalMeshComponent&,
    const Components::MorphTargetComponent, const Components::WorldTransformComponent, const Components::PBRComponent,
    const Components::CSGComponent, const Components::AmbientCubeComponent>;

void SubmitVisibleMeshes(MeshSubmitQuery query, GraphicsDevice rc, std::span<const Entity> mainVisible, std::span<const Entity> shadowVisible) {

    auto IsInList = [](std::span<const Entity> list, Entity e) -> bool { return std::ranges::find(list, e) != list.end(); };

    for (Entity e: query.Entities<Components::MeshComponent>()) {
        bool inMain   = IsInList(mainVisible, e);
        bool inShadow = IsInList(shadowVisible, e);

        if (!inMain && !inShadow) {
            continue;
        }

        auto meshComp = query.Get<Components::MeshComponent>(e);
        if (!meshComp) {
            continue;
        }

        auto gpuMeshOpt = rc.Mesh(meshComp->meshAsset);
        if (!gpuMeshOpt.has_value()) {
            // Explicit cache clears discard lookups, not scene-owned buffers.
            // Rebind the owner's view instead of allocating another mesh.
            if (const auto owned = query.Get<Components::OwnedMeshComponent>(e);
                owned && owned->meshAsset == meshComp->meshAsset && owned->mesh.posBuffer != BufferID::Invalid) {
                rc.RegisterMesh(meshComp->meshAsset, owned->mesh);
                gpuMeshOpt = owned->mesh;
            }
        }
        auto gpuMatOpt = rc.Material(meshComp->materialAsset);
        if (!gpuMeshOpt.has_value() || !gpuMatOpt.has_value()) {
            continue;
        }

        Mesh     gpuMesh = *gpuMeshOpt;
        Material gpuMat  = *gpuMatOpt;

        auto skelMesh   = query.Get<Components::SkeletalMeshComponent>(e);
        auto morphComp  = query.Get<Components::MorphTargetComponent>(e);
        auto worldTrans = query.Get<Components::WorldTransformComponent>(e);

        auto worldMat = Mat4x4::Identity();
        auto prevMat  = Mat4x4::Identity();
        if (worldTrans) {
            worldMat = worldTrans->world;
            prevMat  = worldTrans->previous;
        }

        bool     isSkinned   = skelMesh.has_value();
        uint32_t jointOffset = isSkinned ? skelMesh->jointOffset : 0;

        uint32_t                   morphOffset      = morphComp ? morphComp->offset : 0;
        uint32_t                   activeMorphCount = morphComp ? morphComp->activeCount : 0;
        auto morphWeights = Components::MorphTargetComponent {}.weights;
        if (morphComp) {
            morphWeights = morphComp->weights;
        }

        BufferID scratchVbo = BufferID::Invalid;
        if (isSkinned) {
            scratchVbo = EnsureSkinnedScratch(rc, *skelMesh, gpuMesh.vertexCount);
        }

        // The 3D skybox is ordinary geometry now: it is placed where the
        // importer authored it and submitted with the transform it was given,
        // like every other mesh. It used to be re-based onto the camera here
        // (and, before that, composited into a second depth band), which is
        // what made it occlude and parallax unlike anything else in the world.
        // Nothing about it is special-cased in this loop any more; the flag
        // only survives as far as the TLAS mask and the shadow filter below.
        DrawFlags drawFlags = meshComp->flags;
        if (inMain) {
            drawFlags |= DrawFlags::VisibleInMain;
        }
        if (inShadow) {
            drawFlags |= DrawFlags::VisibleInShadow;
        }

        float roughness = -1.0f;
        float metallic  = -1.0f;
        if (auto pbr = query.Get<Components::PBRComponent>(e)) {
            roughness = pbr->roughness;
            metallic  = pbr->metallic;
        }

        if (auto csg = query.Get<Components::CSGComponent>(e)) {
            CSGDrawParams csgParams;
            csgParams.eyeParams = {
                .transform           = worldMat,
                .prevTransform       = prevMat,
                .cullRadius          = meshComp->cullRadius,
                .localCenter         = {meshComp->localCenter.x, meshComp->localCenter.y, meshComp->localCenter.z},
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
                if (query.IsAlive(mod.operandEntity)) {
                    if (auto cutMesh = query.Get<Components::MeshComponent>(mod.operandEntity)) {
                        auto cutGpuMeshOpt = rc.Mesh(cutMesh->meshAsset);
                        auto cutGpuMatOpt  = rc.Material(cutMesh->materialAsset);
                        if (cutGpuMeshOpt && cutGpuMatOpt) {
                            auto       cutSkelMesh   = query.Get<Components::SkeletalMeshComponent>(mod.operandEntity);
                            auto       cutWorldTrans = query.Get<Components::WorldTransformComponent>(mod.operandEntity);
                            auto cutWorldMat   = Mat4x4::Identity();
                            auto cutPrevMat    = Mat4x4::Identity();
                            if (cutWorldTrans) {
                                cutWorldMat = cutWorldTrans->world;
                                cutPrevMat  = cutWorldTrans->previous;
                            }

                            BufferID cutScratchVbo = BufferID::Invalid;
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

        DrawParams drawParams {
            .transform           = worldMat,
            .prevTransform       = prevMat,
            .cullRadius          = meshComp->cullRadius,
            .localCenter         = {meshComp->localCenter.x, meshComp->localCenter.y, meshComp->localCenter.z},
            .jointOffset         = jointOffset,
            .morphOffset         = morphOffset,
            .activeMorphCount    = activeMorphCount,
            .morphWeights        = morphWeights,
            .flags               = drawFlags,
            .skinnedVertexBuffer = scratchVbo,
            .roughness           = roughness,
            .metallic            = metallic
        };
        if (auto ambient = query.Get<Components::AmbientCubeComponent>(e)) {
            drawParams.hasAmbientCube = true;
            drawParams.ambientCube    = ambient->cube;
        }
        rc.Draw(gpuMat, gpuMesh, drawParams);
    }
}

using ViewCameraQuery = ECS::Query<const Components::CameraComponent, const Components::WorldTransformComponent>;

[[nodiscard]] auto MakeViewportCamera(ViewCameraQuery query, Entity cameraEnt) -> std::optional<Camera> {
    // A viewport camera is built from the main camera's optics: a world without a
    // main camera has no optics to build from, and says so by returning nothing.
    const auto main = query.GetSingleton<Components::CameraComponent>();
    if (!main) {
        return std::nullopt;
    }
    Camera extra = main->camera;
    if (cameraEnt == Entity::Null() || !query.IsAlive(cameraEnt)) {
        return extra;
    }
    if (auto world = query.Get<Components::WorldTransformComponent>(cameraEnt)) {
        const Mat4x4& worldMat = world->world;
        extra.position = {worldMat[0, 3], worldMat[1, 3], worldMat[2, 3]};
    }
    return extra;
}

[[nodiscard]] auto MakeViewFor(ViewCameraQuery query, GraphicsDevice rc, uint64_t frameIndex, Entity cameraEnt, const FrameTarget& target, const ViewportRect& viewport)
    -> std::optional<SceneView> {
    auto cComp = query.Get<Components::CameraComponent>(cameraEnt);

    // The view's camera is the entity's own component when it has one; a bare
    // transform entity gets a viewport camera built from the main camera's optics
    // at that entity's position. An entity with neither has no view to build, and
    // the caller skips it rather than rendering from a camera nobody asked for.
    Camera     cam {};
    if (cComp) {
        cam = cComp->camera;
    } else if (auto fallback = MakeViewportCamera(query, cameraEnt)) {
        cam = *fallback;
    } else {
        return std::nullopt;
    }
    const float      aspect = viewport.height > 0 ? static_cast<float>(viewport.width) / static_cast<float>(viewport.height) : rc.ViewportAspect();
    const auto view = cam.GetViewMatrix();
    const auto proj = cam.GetProjectionMatrix(aspect);
    auto viewProj = proj * view;
    if (cComp) {
        viewProj = cComp->viewProj;
    }

    // The view's planes belong to the view, not to the camera: handed to the
    // renderer with the rest of the SceneView.
    Frustum frustum {};
    frustum.Update(viewProj);

    return SceneView {
        .viewMatrix        = view,
        .projMatrix        = proj,
        .viewProjMatrix    = viewProj,
        .invViewProjMatrix = Inverse(viewProj),
        .worldPosition     = cam.position,
        .viewport          = viewport,
        .target            = target,
        .frustum           = frustum,
        .visibilityMask    = ~0ULL,
        .frameIndex        = static_cast<uint32_t>(frameIndex),
        .time              = static_cast<float>(frameIndex & kFrameClockMask) * kFrameTimeStep,
    };
}

}

std::expected<void, ErrorCode> RenderSystem::Update(Engine& engine, float dt) {
    int        physicsDrawMode = 0;
    auto shadowProjView = Mat4x4::Identity();

    auto mainResult = RenderMain(engine, physicsDrawMode, shadowProjView, dt);
    if (!mainResult) {
        return std::unexpected(mainResult.error());
    }
    if (mainResult->has_value()) {
        return {};
    }

    return {};
}

FrameOutcome<FrameSkipped> RenderSystem::RenderMain(Engine& engine, int& outPhysicsDrawMode, Mat4x4& outShadowProjView, float dt) {
    auto rc = engine.Graphics();
    auto& reg = ZHLN::EngineAccess::RegistryOf(engine);
    const RenderFrameQuery query {reg};
    // The frame's pose, and where "there is nothing to render from" is decided:
    // the component lookup answers emptiness, so the caller checks it before
    // anything is recorded rather than after a dereference.
    const auto camera = query.GetSingleton<Components::CameraComponent>();
    if (!camera) {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }
    // A copy: the pose this frame renders with. Holding a reference into the
    // registry across the whole submit would dangle if that component's storage
    // ever moved.
    const Camera cam            = camera->camera;
    const auto& visibleEntities = engine.GetVisibleEntities();

    Mat4x4 vp {};
    Mat4x4 unjitteredVp {};
    Mat4x4 prevUnjitteredVp {};

    // The getter above already proved a main camera entity exists; this is the
    // entity itself, for the viewport views built below.
    auto cameraEntities = query.Entities<Components::MainCameraTagComponent>();

    // The graphics owner is authoritative; nothing scrapes the registry for it.
    const GraphicsSettings gfx = engine.Graphics().Settings();

    outPhysicsDrawMode = 0;
    if (const auto dbg = query.GetSingleton<Components::DebugSettingsComponent>()) {
        outPhysicsDrawMode = dbg->physicsDrawMode;
    }

    auto scene = engine.Debug();
    scene.SetFrameDelta(dt);
    SubmitParticleEmitters(query.As<ParticleSubmitQuery>(), scene);
    RenderDebug(engine, query.As<RenderDebugQuery>(), outPhysicsDrawMode);

    auto begin_res = rc.BeginFrame();
    if (!begin_res) {
        return std::unexpected(begin_res.error());
    }
    if (!begin_res->has_value()) {
        return FrameSkipped {};
    }
    FrameScope gpuFrame = std::move(**begin_res);
    if (auto env = SyncEnvironmentMap(query.As<EnvironmentQuery>(), rc, engine.Assets()); !env) {
        return std::unexpected(env.error());
    }
    Entity cameraEntity = cameraEntities[0];

    if (auto cComp = query.Get<Components::CameraComponent>(cameraEntity)) {
        vp               = cComp->viewProj;
        unjitteredVp     = cComp->unjitteredViewProj;
        prevUnjitteredVp = cComp->prevUnjitteredViewProj;
    } else {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }

    auto sun = LightingSystem::GetSun(query.As<LightingSystem::SunQuery>());
    if (const auto env = query.GetSingleton<Components::EnvironmentMapComponent>(); env && !HasSceneSun(query.As<SceneSunQuery>())) {
        if (!env->source.empty()) {
            sun.intensity = 0.0f;
        }
    }
    const FVec3 sunDirection = sun.direction;
    // The extracted emitter used to live in EvaluateSH and inherited the
    // environment exposure. Convert its irradiance/pi to directional radiance
    // (pi * irradiance), then apply that same exposure only to cooked suns.
    const float sunIntensity = sun.intensity * (sun.fromEnvironment ? gfx.environment.ambientExposure : 1.0f);
    const FVec3 sunRadiance = sun.color * sunIntensity;

    const float    shadowWidth      = gfx.shadows.width;
    const uint32_t shadowResolution = gfx.shadows.resolution;

    float textelSize = shadowWidth / static_cast<float>(shadowResolution);

    FVec3 shadowCenter = cam.position;
    shadowCenter.x = std::round(shadowCenter.x / textelSize) * textelSize;
    shadowCenter.y = std::round(shadowCenter.y / textelSize) * textelSize;
    shadowCenter.z = std::round(shadowCenter.z / textelSize) * textelSize;

    FVec3  lightPos  = shadowCenter + sunDirection * Shadows::FarOffset;
    const auto lightView = CreateLookAt(lightPos, shadowCenter, {0.0f, 1.0f, 0.0f});

    float      halfWidth = shadowWidth * 0.5f;
    const auto lightProj = CreateOrtho(-halfWidth, halfWidth, -halfWidth, halfWidth, Shadows::NearClip, Shadows::FarDepth);
    outShadowProjView    = lightProj * lightView;

    // The shadow planes are the culler's own: CullingSystem derives them from
    // the same sun and settings, and this matrix is what the passes consume.

    // The frame's scene state, in the engine's own terms: the view, the sun, the
    // sky and the probe. The shader's struct is not this one -- the
    // renderer packs it (RenderContext::SetFrameData) and fills the lanes only it
    // knows, so nothing here is a w-lane of something else.
    const float frameClock = static_cast<float>(engine.GetCurrentFrame() & kFrameClockMask) * kFrameTimeStep;

    FrameData frame {};
    frame.viewProj               = vp;
    frame.unjitteredViewProj     = unjitteredVp;
    frame.prevUnjitteredViewProj = prevUnjitteredVp;
    frame.invViewProj            = Inverse(unjitteredVp);
    frame.camPosition            = cam.position;
    frame.frameClock             = frameClock;
    frame.sunDirection           = sunDirection;
    frame.sunIntensity           = sunIntensity;
    frame.sunRadiance            = sunRadiance;
    frame.probeMin               = gfx.environment.probeMin;
    frame.probeMax               = gfx.environment.probeMax;
    frame.probePos               = gfx.environment.probePos;
    frame.useLocalProbe          = gfx.environment.useLocalProbe;
    frame.enableRTR              = gfx.rayTracing.enableReflections ? 1 : 0;
    frame.fullBright             = gfx.environment.fullBright;
    frame.frameIndex             = static_cast<uint32_t>(engine.GetCurrentFrame());
    frame.shadowWidth            = gfx.shadows.width;
    frame.shadowResolution       = gfx.shadows.resolution;
    frame.sunSize                = gfx.shadows.sunSize;
    frame.ambientExposure        = gfx.environment.ambientExposure;
    frame.skyZenith  = gfx.environment.skyZenith;
    frame.skyHorizon = gfx.environment.skyHorizon;
    frame.skyGround  = gfx.environment.skyGround;

    scene.SetFrameDelta(dt);

    UploadPosePalettes(engine);

    if (outPhysicsDrawMode == 0) {
        SubmitVisibleMeshes(query.As<MeshSubmitQuery>(), rc, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
    }

    if (auto sim_res = gpuFrame.DispatchSimulations(ZHLN::EngineAccess::SceneOf(engine), dt); !sim_res) {
        return std::unexpected(sim_res.error());
    }

    const ViewportRect viewport = rc.Viewport();
    const auto target = engine.AcquireTarget();
    if (!target) {
        return std::unexpected(target.error());
    }
    if (!target->has_value()) {
        return {};
    }
    const FrameTarget attachment = **target;
    const auto       sceneView   = MakeViewFor(query.As<ViewCameraQuery>(), rc, engine.GetCurrentFrame(), cameraEntity, attachment, viewport);
    if (!sceneView) {
        return std::unexpected(RenderSystemError::NoMainCamera);
    }
    if (auto scene_res = gpuFrame.RenderScene(
            SceneRenderPass {
                .view = *sceneView, .camera = cam, .frame = frame, .shadowProjView = outShadowProjView, .settings = gfx
            },
            ZHLN::EngineAccess::SceneOf(engine)
        );
        !scene_res) {
        return std::unexpected(scene_res.error());
    }

    if (const UIDrawData uiData = engine.GetPendingUIData(); !uiData.Empty()) {
        if (auto ui_res = gpuFrame.RenderUI(
                UIView {
                    .viewport   = {},
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

    if (const auto& host = engine.GetPlatformHost(); host.UsesCustomCursor()) {
        const auto     input    = query.GetSingleton<Components::InputStateComponent>();
        const Extent2D hostSize = host.GetSize();
        const float    rawX     = input ? input->mouseX : static_cast<float>(hostSize.width) * 0.5f;
        const float    rawY     = input ? input->mouseY : static_cast<float>(hostSize.height) * 0.5f;
        const float    cursorX  = hostSize.width > 0 ? std::clamp(rawX, 0.0f, static_cast<float>(hostSize.width - 1)) : rawX;
        const float    cursorY  = hostSize.height > 0 ? std::clamp(rawY, 0.0f, static_cast<float>(hostSize.height - 1)) : rawY;

        const CustomCursor cursor(cursorX, cursorY);
        if (auto cursor_res = gpuFrame.RenderUI(
                UIView {
                    .viewport   = {},
                    .target     = attachment,
                    .frameIndex = static_cast<uint32_t>(engine.GetCurrentFrame()),
                },
                cursor.DrawData()
            );
            !cursor_res) {
            return std::unexpected(cursor_res.error());
        }
    }

    if (auto end_res = std::move(gpuFrame).End(); !end_res) {
        return std::unexpected(end_res.error());
    }

    auto& cstats = ZHLN::EngineAccess::WorldOf(engine).GetCullingStats();
    cstats.TotalObjects  = query.Entities<Components::MeshComponent>().size();
    cstats.CulledObjects = cstats.TotalObjects - visibleEntities.size();

    return {};
}

void RenderSystem::RenderDebug(Engine& engine, RenderDebugQuery query, int physicsDrawMode) {
    auto scene = engine.Debug();

    CullingSystem::DrawDebugFrustum(query, scene);

    if (physicsDrawMode > 0) {
        ZHLN::ScopedTimer profTimer("Physics Debug Extract & Upload");

        bool isWireframe = (physicsDrawMode == 1);
        auto debugData   = engine.Physics().DebugDrawData(true, true, isWireframe);

        if (isWireframe) {
            auto UnpackColorVec4 = [](uint32_t packed) {
                float r = static_cast<float>(packed & 0xFF) / 255.0f;
                float g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
                float b = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
                float a = static_cast<float>((packed >> 24) & 0xFF) / 255.0f;
                return FVec4{r, g, b, a};
            };

            for (size_t i = 0; i + 1 < debugData.lineCount; i += 2) {
                const auto& v0 = debugData.lines[i];
                const auto& v1 = debugData.lines[i + 1];
                scene.AddLine({v0.x, v0.y, v0.z}, {v1.x, v1.y, v1.z}, UnpackColorVec4(v0.color), UnpackColorVec4(v1.color));
            }
        } else if (debugData.triangleCount > 0) {
            std::vector<VertexPosition> debugPos;
            std::vector<VertexSurface>  debugSurface;
            debugPos.reserve(debugData.triangleCount);
            debugSurface.reserve(debugData.triangleCount);
            for (size_t i = 0; i < debugData.triangleCount; ++i) {
                const auto& jv = debugData.triangles[i];
                debugPos.push_back({.position = {jv.x, jv.y, jv.z}});
                debugSurface.push_back({.uv = Math::PackUV(0.0f, 0.0f), .color = {.data = jv.color}});
            }
            scene.AddDebugTriangles(std::span {debugPos}, std::span {debugSurface});
        }
    }
}

}

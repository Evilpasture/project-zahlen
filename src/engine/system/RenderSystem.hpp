// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <render/Render.hpp>
#include <expected>

namespace ZHLN {
class Engine;
class PoseUploadQueue;
struct SceneData;
struct CullingStats;
namespace ECS {
class Registry;
}

class RenderSystem {
  public:
    // The presentation step is an ordered engine/frame-plan barrier, not a
    // graph node. This describes its direct component access; helper queries
    // project narrower views without giving those helpers the whole registry.
    // It is not a scheduler access declaration. A future extraction boundary
    // can move scene gathering out of presentation without changing frame order.
    using RenderFrameQuery = ECS::Query<
        const Components::CameraComponent, const Components::MainCameraTagComponent,
        const Components::EnvironmentMapComponent, const Components::MeshComponent,
        Components::SkeletalMeshComponent&, const Components::MorphTargetComponent,
        const Components::WorldTransformComponent, const Components::TransformComponent, const Components::PBRComponent,
        const Components::CSGCutterComponent, const Components::AmbientCubeComponent, const Components::LightComponent,
        const Components::SunTagComponent, const Components::EnvironmentSunTagComponent,
        const Components::ParticleEmitterComponent, const Components::MeshParticleEmitterComponent>;

    static std::expected<void, ErrorCode> Update(
        Engine& engine, ECS::Registry& reg, SceneData& sceneData, PoseUploadQueue& poseUploads, float dt
    );

  private:
    static FrameOutcome<FrameSkipped> RenderMain(
        Engine& engine, ECS::Registry& reg, SceneData& sceneData, PoseUploadQueue& poseUploads,
        Mat4x4& outShadowProjView, float dt
    );

    using RenderDebugQuery = ECS::Query<const Components::CameraComponent>;
    static void RenderDebug(Engine& engine, const CullingStats& cullingStats, RenderDebugQuery query, int physicsDrawMode);
};
}

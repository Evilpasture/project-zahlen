// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "TargetCamera.hpp"
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/GameModule.hpp>
namespace ZHLN {
class Engine;
class InputReader;
namespace FreeCam {
struct FreeCamTagComponent;
}
namespace CameraRig {
class TargetCameraSystem {
  public:
    using RigQuery = ECS::Query<
        TargetCameraComponent&,
        Components::CameraComponent&,
        const Components::WorldTransformComponent,
        const Components::TransformComponent,
        const FreeCam::FreeCamTagComponent>;
    static void Update(RigQuery query, ECS::Res<InputReader> input, FrameDt dt, FrameAlpha alpha) noexcept;
};

auto Describe() -> Module;
void Install(Engine&);
} // namespace CameraRig
} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "helpers/HeadlessEngineFixture.hpp"
#include <Camera/TargetCamera.hpp>
#include <Zahlen/COM/IEntities.hpp>

namespace ZHLN::Test::Headless {

inline void InstallTargetCameraRig(ZHLN::COM::IEngine& engine) {
    engine.ClearScene();
    auto&      camReg    = *engine.Entities();
    const auto camEntity = camReg.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
    if (camEntity) camReg.Patch<ZHLN::Components::CameraComponent>(*camEntity, [](ZHLN::Components::CameraComponent& c) { c.camera = ZHLN::Camera {}; });
    SetPreTickHook(+[](ZHLN::COM::IEngine& eng, float dt) { ZHLN::CameraRig::Step(eng, dt); });
}

} // namespace ZHLN::Test::Headless

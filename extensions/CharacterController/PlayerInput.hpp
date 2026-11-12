// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/PlayerInput.hpp
#pragma once

#include "CharacterComponents.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class InputContext;

namespace Character {

// Device input is latched in Input phase and re-latched after late polling;
// intent translation remains in PlayerIntent before the fixed substeps.
class PlayerInputSystem {
  public:
    using InputQuery = ECS::Query<InputComponent&, const Components::PlayerSeatComponent, const Components::InputStateComponent>;
    using IntentQuery = ECS::Query<MovementComponent&, const InputComponent, const Components::CameraComponent,
                                   const Components::MainCameraTagComponent, const FreeCam::FreeCamTagComponent>;
    static void Update(InputQuery query, ECS::Res<InputContext> input);
    static void PlayerInputTranslate(IntentQuery query);
};

} // namespace Character
} // namespace ZHLN

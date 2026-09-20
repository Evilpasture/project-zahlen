// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/CharacterController/PlayerInput.hpp
#pragma once

namespace ZHLN {

class Engine;
struct Camera;

namespace Character {

// WASD/mouse -> character intent. Update fills per-entity InputComponent
// from the raw InputStateComponent (Input phase); PlayerInputTranslate
// resolves that intent against the resolved camera into world-space
// MovementComponent input (PlayerIntent phase). Moved out of core: the
// key mapping (WASD, Space, LShift, RButton look, 0.15 sensitivity) is
// this controller's contract, not engine substrate.
class PlayerInputSystem {
  public:
    void Update(Engine& engine);
    void PlayerInputTranslate(Engine& engine, const Camera& cam);
};

} // namespace Character
} // namespace ZHLN

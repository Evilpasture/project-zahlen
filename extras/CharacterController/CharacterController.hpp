// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/CharacterController/CharacterController.hpp
//
// Overgrowth-style character locomotion: WASD intent, camera-relative
// translation, jump/gravity integration, sprint, yaw slewing. Core keeps
// PhysicsContext::CreateCharacter / CharacterVirtual and the raw
// InputStateComponent; everything that interprets keys into character
// movement lives here.
#pragma once

#include "CharacterComponents.hpp"
#include "CharacterMovement.hpp"
#include "PlayerInput.hpp"

namespace ZHLN {

class Engine;

namespace Character {

/// Composition-root entry point: registers the components, installs the
/// physics-substep hooks, the free-cam speed query, and contributes the
/// two frame steps plus the orientation-interpolation graph node through
/// the engine's extension seams. Call once after Engine::Create; the seams
/// replay the contributions on every schedule/graph rebuild (scene resets).
///
/// Install this BEFORE any layer whose systems read MovementComponent
/// (Interaction finds the player through it): its external-writes anchor
/// must be registered ahead of those readers for hazard analysis.
void Install(Engine& engine);

} // namespace Character
} // namespace ZHLN

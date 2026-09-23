// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Interaction/InteractionSystem.hpp
#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;
struct SystemContext;

namespace ECS {
class SystemGraph;
} // namespace ECS

namespace Interaction {

// E-key proximity interaction: trigger volumes detect the player, pickups
// move into the player's container, usables dispatch their script hash.
// Moved out of core because the whole model (inventory slots, pickup flags,
// script hashes) is RPG/adventure gameplay, not engine substrate.
class InteractionSystem {
  public:
    InteractionSystem()  = default;
    ~InteractionSystem() = default;

    void Update(SystemContext& ctx, float dt);
};

// Composition-root entry point: registers the interaction components with
// the engine's registry and contributes InteractionSystem to the update
// graph. The contribution replays on every graph rebuild (scene resets),
// so calling this once after Engine::Create is enough.
void Install(Engine& engine);

} // namespace Interaction
} // namespace ZHLN

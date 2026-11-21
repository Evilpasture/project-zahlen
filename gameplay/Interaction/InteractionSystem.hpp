// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "InteractionComponents.hpp"
#include <Zahlen/Common.h>

namespace ZHLN::COM {
struct IActionContext;
struct IAudioDevice;
struct IEngine;
struct IEntities;
struct ISimulation;
} // namespace ZHLN::COM

namespace ZHLN::Interaction {

struct UseIntent {
    bool use        = false;
    bool wasUseDown = false;

    [[nodiscard]] constexpr auto JustPressed() const noexcept -> bool {
        return use && !wasUseDown;
    }
};

// Host-side E-key proximity and trigger volume interaction over COM::IEntities.
class ZHLN_STATIC_API InteractionSystem {
  public:
    using UseIntent = Interaction::UseIntent;

    static void Update(COM::IEntities& entities, UseIntent useIntent = {}, COM::ISimulation* physics = nullptr,
                       COM::IAudioDevice* audio = nullptr);
};

// Linear (sliding) and angular (rotating) door movement over COM::IEntities.
class ZHLN_STATIC_API DoorMovementSystem {
  public:
    static void Update(COM::IEntities& entities, float dt, COM::ISimulation* physics = nullptr,
                       COM::IAudioDevice* audio = nullptr);
};

// Button depression, wait timer, return animation and output firing over COM::IEntities.
class ZHLN_STATIC_API ButtonMovementSystem {
  public:
    static void Update(COM::IEntities& entities, float dt, COM::ISimulation* physics = nullptr,
                       COM::IAudioDevice* audio = nullptr);
};

ZHLN_STATIC_API void Step(COM::IEngine& engine, float dt, bool& wasUseDown);

struct Layer {
    bool wasUseDown = false;

    void Tick(COM::IEngine& engine, float dt) {
        Step(engine, dt, wasUseDown);
    }
    void Stop(COM::IEngine&) noexcept {
        wasUseDown = false;
    }
};

} // namespace ZHLN::Interaction

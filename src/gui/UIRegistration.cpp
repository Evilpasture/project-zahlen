// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/gui/UIRegistration.cpp
//
// The GUI subsystem's half of component registration. Engine registers
// ZHLN::Components (transform, physics, render, audio, ...); the widget types
// are registered here, next to the systems that read them.
//
// RegisterAllComponentsIn walks UIComponents with static reflection, so this
// file never lists a component by hand: adding a nested struct to
// Zahlen/gui/UIComponents.hpp registers it on the next build.
#include "Zahlen/gui/UIComponents.hpp"

#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN::GUI {

void RegisterUIComponents(ECS::Registry& reg) {
    reg.RegisterAllComponentsIn<UIComponents>();
}

} // namespace ZHLN::GUI

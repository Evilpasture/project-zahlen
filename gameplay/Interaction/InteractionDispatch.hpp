// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "InteractionComponents.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN::Interaction {

// Output dispatch can synchronously fire an input on any named entity, which
// may in turn fire more outputs. All the families it can touch must appear in
// the caller's graph signature, including output counters and light state.
using DispatchQuery = ECS::Query<
    const Components::NameComponent, DoorComponent&, ButtonComponent&,
    TriggerComponent&, Components::LightComponent&, const UsableComponent,
    EntityConnectionComponent&>;

} // namespace ZHLN::Interaction

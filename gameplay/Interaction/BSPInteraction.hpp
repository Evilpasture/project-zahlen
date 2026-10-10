// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "InteractionComponents.hpp"
#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPRead.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <string_view>

namespace ZHLN {
class AudioContext;

namespace Interaction {

// Dispatches a named action (e.g. "Open", "Close", "Toggle", "Press", "Lock")
// to all entities matching `targetname`.
void FireEntityInput(
    ECS::Registry&                registry,
    std::string_view              targetname,
    std::string_view              input,
    std::string_view              parameter = "",
    ZHLN::Optional<AudioContext&> audio     = std::nullopt
);

// Evaluates and fires entity outputs for a specific event ("OnPressed", "OnOpen", etc.)
void FireEntityOutputs(ECS::Registry& registry, Entity entity, std::string_view event, ZHLN::Optional<AudioContext&> audio = std::nullopt);

// Reads BSPEntity keyvalues from map.entities and attaches DoorComponent,
// ButtonComponent, TriggerComponent, EntityConnectionComponent, and
// BSPEntityMetadataComponent to corresponding spawned entities.
void InitializeBSPEntities(ECS::Registry& registry, const BSP::BSPMap& map, const Scene::Instance& instance, const BSP::ImportOptions& options = {});

} // namespace Interaction
} // namespace ZHLN

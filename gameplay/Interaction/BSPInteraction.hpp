// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// gameplay/Interaction/BSPInteraction.hpp
//
// The gameplay half of the map's interactive entities: what a door *does*, and
// the runtime dispatch that makes it do it.
//
// The format half -- class names, spawnflag bits, key names, the 0x1B output
// separators, the movedir sentinels -- moved down to plugins/BSP (see
// <BSP/BSPInteraction.hpp>, which is where DescribeInteractiveEntities lives).
// It had to: a plugin may not include a gameplay header, so the file that
// *decodes a map* can live below this layer and the file that *builds
// DoorComponent* cannot. This header is the seam between them, and every
// caller keeps the same one-function entry point it always had.

#include "InteractionComponents.hpp"
#include "InteractionDispatch.hpp"
#include <BSP/BSPGeometry.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Engine/Scene.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <string_view>

namespace ZHLN {
class AudioContext;
class Engine;

namespace Interaction {

// Runtime dispatch declares every family touched by recursive input/output
// delivery. The entity overload also handles unnamed brush entities.
void FireEntityInput(DispatchQuery query, std::string_view targetname, std::string_view input,
                     std::string_view parameter = "", ZHLN::Optional<AudioContext&> audio = std::nullopt);
void FireEntityInput(DispatchQuery query, Entity entity, std::string_view input,
                     std::string_view parameter = "", ZHLN::Optional<AudioContext&> audio = std::nullopt);
uint32_t FireEntityOutputs(DispatchQuery query, Entity entity, std::string_view event,
                           ZHLN::Optional<AudioContext&> audio = std::nullopt);

// Translates a map's interactive entities into this layer's components:
// DoorComponent, ButtonComponent, TriggerComponent, EntityConnectionComponent
// and BSPEntityMetadataComponent on the entities the instantiator already
// spawned, plus one lightweight entity per info_teleport_destination.
//
// No parsing happens here. The map is described once by
// BSP::DescribeInteractiveEntities, and this turns each record into components
// -- which is where the game's own defaults live (a door's 100 unit/s, a
// button's 3 s reset, a trigger's 0.2 s re-arm), because those are rules of
// this game, not conventions of the file format.
void InitializeBSPEntities(ECS::Registry& registry, const BSP::BSPMap& map, const Scene::Instance& instance, const BSP::ImportOptions& options = {});
void InitializeBSPEntities(Engine& engine, const BSP::BSPMap& map, const Scene::Instance& instance, const BSP::ImportOptions& options = {});

} // namespace Interaction
} // namespace ZHLN

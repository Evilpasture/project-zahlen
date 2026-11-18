// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// gameplay/Interaction/BSPInteraction.hpp
//
// The gameplay half of the map's interactive entities: what a door *does*, and
// the runtime dispatch that makes it do it, driven purely through COM.

#include "InteractionComponents.hpp"
#include <BSP/BSPGeometry.hpp>
#include <Scene/Scene.hpp>
#include <Zahlen/Common.h>
#include <string_view>

namespace ZHLN::COM {
struct IAudioDevice;
struct IEngine;
struct IEntities;
} // namespace ZHLN::COM

namespace ZHLN::Interaction {

// Runtime I/O dispatch over COM::IEntities.
ZHLN_STATIC_API void     FireEntityInput(COM::IEntities& entities, std::string_view targetname, std::string_view input,
                                         std::string_view parameter = "", COM::IAudioDevice* audio = nullptr);
ZHLN_STATIC_API void     FireEntityInput(COM::IEntities& entities, Entity entity, std::string_view input,
                                         std::string_view parameter = "", COM::IAudioDevice* audio = nullptr);
ZHLN_STATIC_API uint32_t FireEntityOutputs(COM::IEntities& entities, Entity entity, std::string_view event,
                                           COM::IAudioDevice* audio = nullptr);

// Translates decoded BSP interactive records into Interaction components.
ZHLN_STATIC_API void InitializeBSPEntities(COM::IEntities& entities, const BSP::BSPMap& map,
                                           const Scene::Instance& instance, const BSP::ImportOptions& options = {});
ZHLN_STATIC_API void InitializeBSPEntities(COM::IEngine& engine, const BSP::BSPMap& map,
                                           const Scene::Instance& instance, const BSP::ImportOptions& options = {});

} // namespace ZHLN::Interaction

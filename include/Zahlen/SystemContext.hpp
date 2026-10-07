// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The name survives for extension entry points and for code that predates the
// split: `SystemContext` is now the simulation domain's *carrier* -- the
// registry, the simulation graph's services, and this execution's frame. It is
// not a bag of engine pointers, and it cannot grow into one: the services it
// exposes are the members of SimServices, and a system can only reach the ones
// its graph was built with.

#include <Zahlen/EngineServices.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/ecs/Carrier.hpp>

namespace ZHLN {

using SystemContext = ECS::Carrier<SimServices>;
using SimGraph      = ECS::SystemGraph<SimServices>;
using RenderGraph   = ECS::SystemGraph<RenderServices>;

} // namespace ZHLN

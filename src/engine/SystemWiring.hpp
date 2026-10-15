// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {

class Engine;

// Core's own frame steps. Runs once per engine; see the definition.
void BuildCoreFramePlan(Engine& engine);
void BuildSystemGraphs(Engine& engine);

auto InitializeDefaultScene(Engine& engine) -> bool;

}

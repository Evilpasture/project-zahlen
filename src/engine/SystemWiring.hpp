// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {

class Engine;

void BuildFrameScheduler(Engine& engine);
void BuildSystemGraphs(Engine& engine);

/// Registers all components, creates the default camera/settings/UI
/// singletons and compiles both graphs plus the frame schedule. Engine
/// infrastructure (not fallback content) -- what Engine::InitializeDefaultScene
/// forwards to.
auto InitializeDefaultScene(Engine& engine) -> bool;

} // namespace ZHLN

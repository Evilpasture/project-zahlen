// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ALife stress hooks for TestPerformance.
//
// The first-party ALife toolkit declares `ZHLN::Components` as a *namespace*
// (extras/ALife/ALifeComponents.hpp) while the core engine declares it as a
// *struct* (include/Zahlen/Components.hpp). A translation unit can never see
// both declarations, so all ALife-specific test code lives in PerfALife.cpp
// (which includes the ALife headers but NOT the core Components header) and
// TestPerformance.cpp talks to it through this minimal interface.
//
// Component identity across the two translation units is safe: the ECS dense
// ID is derived from the consteval C++26-reflection type name, and the
// hash -> dense ID table is process-global inside zahlen_engine.

#pragma once

#include <Zahlen/Engine.hpp>
#include <cstdint>

namespace ZHLN::Perf {

// Creates the creature population (faction hostility, callbacks, waypoints)
// and the ALife Simulator. Must be called once, after InitializeDefaultScene().
[[nodiscard]] bool ALife_Init(Engine& engine, uint32_t creatureCount);

// Drives one frame of ALife stress: waypoint movement (fiber fan-out) plus the
// simulator's parallel phases (think/state-switch + spatial-grid rebuild +
// offline faction interactions).
void ALife_DriveFrame(Engine& engine);

// Total ALife events (state changes, deaths, ...) observed so far.
[[nodiscard]] uint64_t ALife_EventCount();

} // namespace ZHLN::Perf

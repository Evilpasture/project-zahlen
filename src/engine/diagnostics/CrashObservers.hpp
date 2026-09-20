// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/CrashObservers.hpp
//
// Subsystem registration for the crash observer bus. Engine-private: the types themselves
// (CrashObserver, CrashObserverEntry, CrashState) are public in <Zahlen/Core/CrashState.hpp>
// because CrashState embeds the registry array, but registering a dump is an engine-internal
// act.
//
// A crash dump wants to know what the subsystems were doing, yet the code running inside a
// signal handler must not #include the subsystems it dumps -- so each subsystem registers a
// dump routine during Engine initialisation and the crash handler iterates the registry,
// instead of CrashHandler.cpp pulling in Engine, Camera and Physics.
//
// Registration is neither signal-safe nor thread-safe, deliberately: it happens once, on the
// main thread, while the engine is being built, long before anything can fault.

#pragma once

#include <Zahlen/Core/CrashState.hpp>
#include <string_view>

namespace ZHLN::Diagnostics {

/// Adds a subsystem dump routine to `state`; false when the registry is full or the observer is
/// null, which costs that subsystem's dump and nothing else.
///
/// `name` is printed as a section header before the observer runs, so a crash log says which
/// subsystem was being dumped when a secondary fault happened. It is a non-owning string_view,
/// so it must outlive the registration -- a string literal, which is what every caller uses.
auto RegisterCrashObserver(CrashState& state, std::string_view name, CrashObserver observer, void* context) noexcept -> bool;

/// Drops every registration in `state`.
///
/// Call this before the subsystems go away, not after: an observer holds a raw
/// pointer to a Camera or a PhysicsContext, and a crash during teardown would
/// otherwise dump freed memory.
void ClearCrashObservers(CrashState& state) noexcept;

/// Writes to the crash log from inside an observer.
///
/// Provided here so a subsystem registering an observer needs this one header
/// and nothing else from the diagnostics layer. It is a raw, unformatted write:
/// format into your own buffer or with ZHLN::Format first, then hand the text
/// over. Nothing on this path allocates.
void WriteCrashOutput(std::string_view text) noexcept;

} // namespace ZHLN::Diagnostics

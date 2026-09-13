// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/CrashObservers.hpp
//
// The crash observer bus. A crash dump wants to know what the subsystems were
// doing, but the code that runs inside a signal handler must not #include the
// subsystems it dumps: diagnostics/CrashHandler.cpp used to pull in
// <Zahlen/Engine.hpp>, <Zahlen/Camera.hpp> and <Zahlen/physics/Physics.hpp> so
// it could reach into Camera::frustum and PhysicsContext directly, which made
// the crash path depend on the whole engine and made it impossible to add a
// subsystem dump without editing the crash handler.
//
// Instead each subsystem registers a dump routine during Engine
// initialisation and the crash handler iterates the registry. The registry is a
// fixed-capacity array of function pointers with an atomic count -- no
// container, no allocation, nothing that a signal can interrupt mid-resize --
// so walking it from a handler is safe.
//
// Registration is not signal-safe and not thread-safe. That is deliberate: it
// happens once, on the main thread, while the engine is being built, long before
// anything can fault.

#pragma once

#include <Zahlen/Core/SignalSafe.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::Diagnostics {

/// Dumps one subsystem's state during a crash.
///
/// `context` is whatever the registrant passed to RegisterCrashObserver, which
/// is how a member function gets here: register a captureless lambda (which
/// converts to this pointer type) that casts `context` back to the subsystem.
///
/// Called from a signal / VEH context. Implementations should assume the process
/// is already dying: do not take locks that the faulting thread may hold, do not
/// allocate where it can be avoided, and never throw.
using CrashObserver = void (*)(void* context, const SignalEvent& event) noexcept;

/// Room for eight subsystems. Fixed, because a std::vector here would allocate
/// inside the thing that runs when the allocator may be the thing that broke.
inline constexpr size_t kMaxCrashObservers = 8;

/// Adds a subsystem dump routine. Returns false when the registry is full or the
/// arguments are unusable; a failed registration costs that subsystem's dump and
/// nothing else.
///
/// `name` is printed as a section header before the observer runs, so a crash
/// log says which subsystem was being dumped when a secondary fault happened.
auto RegisterCrashObserver(std::string_view name, CrashObserver observer, void* context) noexcept -> bool;

/// Drops every registration.
///
/// Call this before the subsystems go away, not after: an observer holds a raw
/// pointer to a Camera or a PhysicsContext, and a crash during teardown would
/// otherwise dump freed memory.
void ClearCrashObservers() noexcept;

/// Writes to the crash log from inside an observer.
///
/// Provided here so a subsystem registering an observer needs this one header
/// and nothing else from the diagnostics layer. It is a raw, unformatted write:
/// format into your own buffer or with ZHLN::Format first, then hand the text
/// over. Nothing on this path allocates.
void WriteCrashOutput(std::string_view text) noexcept;

} // namespace ZHLN::Diagnostics

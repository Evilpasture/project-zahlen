// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/EngineFactory.hpp
//
// The one symbol a consumer has to resolve out of libzahlen_engine. It is an
// ordinary C++ function: GCC and Clang both implement the Itanium C++ ABI, so
// the mangled name, the vtable layout and the calling convention agree without
// an extern "C" shim or a hand-written dispatch struct.
//
// The engine is allocated, reference counted and destroyed inside the library.
// Nothing here hands the consumer memory to free.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Ref.hpp>

#include <expected>

namespace ZHLN::COM {

// Returns a raw pointer rather than Ref<IEngine>: Result<T> carries T across
// the boundary by value and so requires a trivially copyable T, which a
// reference-counting handle is not. The reference is already taken -- adopt
// it, do not AddRef it.
[[nodiscard]] ZHLN_API auto CreateEngine(const EngineConfigCOM& config) noexcept -> Result<IEngine*>;

// What callers actually want. This is inline and compiles in the consumer's
// own translation unit, so std::expected is free to appear here: it is built
// and consumed on one side of the boundary and never travels across it.
// Result<Ref<IEngine>> would not even compile -- Ref is not trivially
// copyable, which is exactly the rule Result enforces.
[[nodiscard]] inline auto CreateEngineRef(const EngineConfigCOM& config = {}) noexcept
    -> std::expected<Ref<IEngine>, ErrorCode> {
    const Result<IEngine*> created = CreateEngine(config);
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<IEngine>::Adopt(created.value);
}

} // namespace ZHLN::COM

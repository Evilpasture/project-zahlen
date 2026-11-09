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
#include <Zahlen/COM/IEngineBuilder.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Ref.hpp>

#include <expected>

namespace ZHLN::COM {

// Returns a raw pointer rather than Ref<IEngine>: Result<T> carries T across
// the boundary by value and so requires a trivially copyable T, which a
// reference-counting handle is not. The reference is already taken -- adopt
// it, do not AddRef it.
// The one exported entry point. Creation options are set on the returned
// builder rather than passed as a struct -- see IEngineBuilder.hpp for why a
// by-reference config struct could not survive a field being added to it.
[[nodiscard]] ZHLN_API auto CreateEngineBuilder() noexcept -> Result<IEngineBuilder*>;

// What callers actually want. Inline, so std::expected is built and consumed
// in the consumer's own translation unit and never travels across the
// boundary. Result<Ref<T>> would not compile -- Ref is not trivially
// copyable, which is exactly the rule Result enforces.
[[nodiscard]] inline auto CreateEngineBuilderRef() noexcept -> std::expected<Ref<IEngineBuilder>, ErrorCode> {
    const Result<IEngineBuilder*> created = CreateEngineBuilder();
    if (!created.HasValue()) return std::unexpected(created.error);
    return Ref<IEngineBuilder>::Adopt(created.value);
}

// Defaults all the way through, for the common case.
[[nodiscard]] inline auto CreateEngineRef() noexcept -> std::expected<Ref<IEngine>, ErrorCode> {
    auto builder = CreateEngineBuilderRef();
    if (!builder.has_value()) return std::unexpected(builder.error());
    return (*builder)->BuildRef();
}

} // namespace ZHLN::COM

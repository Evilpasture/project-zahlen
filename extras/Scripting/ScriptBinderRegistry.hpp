/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/Scripting/ScriptBinderRegistry.hpp
//
// Populates the ScriptBinder registry from core's Components.
//
// ScriptBinder is reflection-driven: Register<T>() walks T's fields and methods
// and builds the property accessors and invocation thunks that let a script
// reach them by name. Until something calls it the registry is empty, and every
// lookup in ScriptECSBridge fails with TypeNotFound -- which is the state this
// replaces.
//
// The set of registered types is derived the same way the FFI layouts are, by
// walking ForEachNestedType<Components>, so a component added to core becomes
// scriptable on the next build with no edit here. The registry key is
// Reflect::TypeName<T>(), which is the bare identifier -- the same spelling
// GenFFICdef gives the C struct, so one name identifies a component to Lua, to
// ffi.cdef and to the binder alike.

#pragma once

// ScriptValueTypes.hpp must be included before Register<T>() is instantiated
// below. ScriptBinder's conversions reach the ScriptValueTrait specializations
// at instantiation time, so a trait declared after this point would simply never
// be consulted and the engine's value types would silently fall back to opaque
// boxing. Including it here rather than in ScriptBinder.hpp keeps Jolt out of
// the generic binder.
#include <Scripting/ScriptBinder.hpp>
#include <Scripting/ScriptValueTypes.hpp>

#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection.hpp>

#include <cstddef>

namespace ZHLN {

/// Register every component type core declares. Returns how many were added.
///
/// Safe to call more than once; Register overwrites the entry for a type rather
/// than duplicating it.
inline auto RegisterCoreScriptTypes() -> std::size_t {
    auto&      binder = ScriptBinder::Get();
    std::size_t count = 0;

    Reflect::ForEachNestedType<Components>([&]<typename Component>() {
        binder.Register<Component>();
        ++count;
    });

    return count;
}

} // namespace ZHLN

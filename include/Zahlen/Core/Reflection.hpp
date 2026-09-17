// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection.hpp
//
// The umbrella. Every module under Reflection/ in one include, which is what
// this path has always been: anything that includes it is unaffected by the
// split, so it stays as the "I want reflection, all of it" header (the module
// interface units, the reflection test, the odd tool).
//
// Everything else in the tree now includes the one module it needs instead:
//
//   Reflection/Core.hpp         primitives, feature check, TypeName      <meta>
//   Reflection/Enums.hpp        enumerators, string <-> enum, messages
//   Reflection/Annotations.hpp  P3394 tags: predicates, values, walks
//   Reflection/Structs.hpp      fields, names, offsets, member walks
//   Reflection/Class.hpp        bases, member functions, nested types
//   Reflection/Dynamic.hpp      TypeDescriptor/AggregateBuilder/Define
//   Reflection/Utilities.hpp    generic compare/hash/copy, ToDebugString, <format>
//
// Include cost is the reason the directory exists: Error.hpp and ErrorCode.hpp
// take Enums.hpp (which carries Core.hpp) and get TypeName, the category
// registry and the message tables without <format> or a single member-reflected
// template; TOML.hpp takes Enums.hpp plus Structs.hpp for field iteration and
// never sees the dynamic builders or the formatter; SignalSafetyInspector.hpp
// takes Annotations.hpp alone.
//
// Each module carries both halves of its own configuration -- the real
// definition, and the degraded stand-in used when the compiler has no P2996 --
// because a translation unit that includes one module has to work either way.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Dynamic.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>

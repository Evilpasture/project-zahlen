// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection.hpp
//
// The umbrella: every module under Reflection/ in one include, for the callers that want all
// of it (the module interface units, the reflection test, the odd tool). Everything else
// includes the one module it needs:
//
//   Reflection/Core.hpp         primitives, feature check, TypeName      <meta>
//   Reflection/Enums.hpp        enumerators, string <-> enum, messages
//   Reflection/Annotations.hpp  P3394 tags: predicates, values, walks
//   Reflection/Structs.hpp      fields, names, offsets, member walks
//   Reflection/Class.hpp        bases, member functions, nested types
//   Reflection/Dynamic.hpp      TypeDescriptor/AggregateBuilder/Define
//   Reflection/Utilities.hpp    generic compare/hash/copy, ToDebugString, <format>
//
// Include cost is why the directory exists: Error.hpp and ErrorCode.hpp take Enums.hpp and get
// TypeName plus the message tables without <format>; TOML.hpp takes Enums.hpp and Structs.hpp;
// SignalSafetyInspector.hpp takes Annotations.hpp alone. Each module carries both halves of its
// configuration -- the real definition and the no-P2996 stand-in -- because a unit including
// one module has to work either way.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Dynamic.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>

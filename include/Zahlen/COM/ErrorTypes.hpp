// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ErrorTypes.hpp
//
// Fixed-layout wire types for IErrorRegistry. Everything here is
// standard-layout and trivially copyable so it can cross the shared-library
// boundary in chain nodes and interface calls.

#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/String.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::COM {

struct IErrorRegistry;

// One enumerator of a registered error category. The registry copies the
// strings; the views only need to outlive the RegisterCategory call.
struct ErrorEnumEntry {
    uint32_t   value = 0;
    StringView name {};
    StringView message {};
};

// ---- Wire logging primitives ----------------------------------------------
// Declared here, in COM, and NOT aliased from Zahlen/Log.hpp: the COM surface
// must stay includable, auditable and versionable on its own, and a COM
// consumer must not acquire Layer 0 (source_location, std::format, the ambient
// sink) by including a logger header. Values match ZHLN::LogSeverity and
// ZHLN::LogSource one for one; src/engine/com/LoggerHub.cpp asserts that.
enum class LogSeverity : uint8_t { Debug, Info, Warning, Error, Off };

struct LogSource {
    StringView file {};
    StringView function {};
    //: Open channel name ("Vulkan", "DeviceLost", "MyPlugin"). Empty when the
    //: call site named none. A *view*: it points into the recording code's
    //: storage, so anything that outlives that code copies it (see ErrorRecord).
    StringView channel {};
    uint32_t   line = 0;
};

static_assert(std::is_standard_layout_v<LogSource> && std::is_trivially_copyable_v<LogSource>);

// COM::LogContext lives in LoggerTypes.hpp alongside COM::LogSeverity.
// Zahlen/COM/LoggerTypes.hpp, beside COM::LogSeverity: the two travel together
// through ILogger::Write, and neither of them is an alias of a Layer-0 type.
// This header no longer includes Zahlen/Log.hpp at all.

// Filled by IErrorRegistry::Describe through an out-chain. Strings longer
// than the fixed capacity are truncated, never allocated.
struct ErrorDescription {
    String64  category {};
    String64  name {};
    String256 message {};
};

// The most recent RecordDetail call on a registry: the code plus the human
// context ("part 3: activeMorphCount=6 > 4") a returning function attached to
// it, and where it was recorded.
//
// Every string here is a fixed-capacity member owned by the payload, not a view
// into the recording DSO. That is the whole point of the type: the reason a
// plugin calls RecordDetail before it fails is that its code is about to be
// unloaded, so a payload that borrowed its .rodata would dangle precisely when
// the record is read. Paths longer than the capacity are truncated, like every
// other wire string (see ErrorDescription); nothing is allocated.
struct ErrorRecord {
    ErrorCode code {};
    String256 detail {};
    String64  file {};
    String128 function {};
    uint32_t  line   = 0;
    //: Owned copy of LogSource::channel -- see the note above: a plugin's
    //: channel literal must not outlive the plugin.
    String32  channel {};
};

// Chain node carrying a caller-owned registry into CreateEngineRaw /
// CreateLogicWorldRaw / CreateAppLayerRaw. Null means "create a default".
struct ErrorConfig {
    IErrorRegistry* registry = nullptr;
};

static_assert(std::is_standard_layout_v<ErrorEnumEntry> && std::is_trivially_copyable_v<ErrorEnumEntry>);
static_assert(std::is_standard_layout_v<LogSource> && std::is_trivially_copyable_v<LogSource>);
static_assert(std::is_standard_layout_v<ErrorDescription> && std::is_trivially_copyable_v<ErrorDescription>);
static_assert(std::is_standard_layout_v<ErrorRecord> && std::is_trivially_copyable_v<ErrorRecord>);
static_assert(std::is_standard_layout_v<ErrorConfig> && std::is_trivially_copyable_v<ErrorConfig>);

} // namespace ZHLN::COM

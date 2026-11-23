// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ErrorTypes.hpp
//
// Fixed-layout wire types for IErrorRegistry. Everything here is
// standard-layout and trivially copyable so it can cross the shared-library
// boundary in chain nodes and interface calls.

#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Log.hpp>
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

// LogSource lives in Core (Zahlen/Log.hpp); this alias keeps the COM
// spelling working for ABI-facing declarations.
using LogSource = ZHLN::LogSource;

// Filled by IErrorRegistry::Describe through an out-chain. Strings longer
// than the fixed capacity are truncated, never allocated.
struct ErrorDescription {
    String64  category {};
    String64  name {};
    String256 message {};
};

// The most recent RecordDetail call on a registry: the code plus the
// human context ("part 3: activeMorphCount=6 > 4") a returning function
// attached to it.
struct ErrorRecord {
    ErrorCode  code {};
    String256  detail {};
    LogSource  source {};
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

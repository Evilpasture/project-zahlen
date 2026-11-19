// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Console/Console.hpp>
#include <Zahlen/Common.h>
#include <string_view>

namespace ZHLN {
namespace COM {
struct IEngine;
struct IEntities;
} // namespace COM
class ScriptComponentAccess;

class ZHLN_STATIC_API ConsoleDebugger {
  public:
    static void Execute(COM::IEngine& engine, GameConsole& console, std::string_view commandLine);
    static void Execute(COM::IEntities& entities, GameConsole& console, std::string_view commandLine);
    static void Execute(ScriptComponentAccess inspection, GameConsole& console, std::string_view commandLine);
};

} // namespace ZHLN

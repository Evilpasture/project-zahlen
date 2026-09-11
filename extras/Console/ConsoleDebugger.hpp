// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Console/Console.hpp>
#include <Zahlen/Common.h>
#include <string_view>

namespace ZHLN {
class Engine;
namespace ECS {
class Registry;
}

class ZHLN_API ConsoleDebugger {
  public:
    static void Execute(Engine& engine, GameConsole& console, std::string_view commandLine);
    static void Execute(ECS::Registry& registry, GameConsole& console, std::string_view commandLine);
};

} // namespace ZHLN

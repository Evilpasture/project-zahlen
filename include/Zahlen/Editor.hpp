// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API WorldEditor {
  public:
    /**
     * @brief Launches the interactive World Editor loop.
     *        Provides ImGui Entity Hierarchy, Component Inspector,
     *        Viewport Ray-Picking, and Play/Pause simulation controls.
     */
    static int Run(Engine& engine, const CommandLineOptions& options);
};

} // namespace ZHLN

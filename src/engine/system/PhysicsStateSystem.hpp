// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {
class Engine;

class ZHLN_API PhysicsStateSystem {
  public:
    static void WriteBack(Engine& engine) noexcept;
};

class ZHLN_API VisualInterpolationSystem {
  public:
    static void Update(Engine& engine, float alpha) noexcept;
};

} // namespace ZHLN

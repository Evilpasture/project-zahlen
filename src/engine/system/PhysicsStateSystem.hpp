// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {
class Engine;
struct SystemContext;

class ZHLN_API PhysicsStateSystem {
  public:
    static void Reconcile(Engine& engine) noexcept;
};

class ZHLN_API VisualInterpolationSystem {
  public:
    static void Update(SystemContext& ctx) noexcept;
};

}

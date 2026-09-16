// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {
class Engine;
struct SystemContext;

class ZHLN_API PhysicsStateSystem {
  public:
    /// Reclaims physics slots whose ECS owner was removed outside DespawnEntity.
    static void Reconcile(Engine& engine) noexcept;
};

class ZHLN_API VisualInterpolationSystem {
  public:
    /// Interpolates physics poses into TransformComponent at ctx.alpha. Runs
    /// inside the update graph, so it consumes a SystemContext.
    static void Update(SystemContext& ctx) noexcept;
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Config.hpp>
namespace ZHLN::COM {
struct LogicWorldInfo {
    PhysicsConfig physics {};
    uint32_t      activePhysicsBodies = 0;
};
// A renderer/window/audio-free engine-owned runtime. Capabilities are borrowed
// from this reference-counted owner, or Query/Ref::As may retain them separately.
// One host thread drives it. No registry, World or Jolt object crosses COM.
struct ILogicWorld: IObject {
    [[nodiscard]] virtual IEntities*   Entities() noexcept                   = 0;
    [[nodiscard]] virtual ISimulation* Simulation() noexcept                 = 0;
    virtual Result<void> GetInfo(ChainOutNode*) const noexcept = 0;
    // Flush deferred hierarchy destruction, step physics once. No fixed clock,
    // ECS transform sync, graphics, event polling or input evaluation is hidden.
    virtual Result<void> Advance(float dt) noexcept = 0;
    virtual void                       Flush() noexcept           = 0;
    virtual void                       Clear() noexcept           = 0;

  protected:
    ~ILogicWorld() = default;
};
} // namespace ZHLN::COM

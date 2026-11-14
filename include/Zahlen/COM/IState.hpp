// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IState.hpp
//
// Engine-level state and culling diagnostics across the COM boundary.

#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/Profiler.hpp>

#include <type_traits>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<ZHLN::CullingStats> && std::is_trivially_copyable_v<ZHLN::CullingStats>);

struct IState : IObject {
    [[nodiscard]] virtual ZHLN::CullingStats CullingStats() const noexcept = 0;
    virtual void                             ResetCullingStats() noexcept = 0;
    virtual void                             SetCullingEnabled(bool enabled) noexcept = 0;
    virtual void                             SetFreezeFrustum(bool freeze) noexcept = 0;
    [[nodiscard]] virtual int                PhysicsDrawMode() const noexcept = 0;
    virtual void                             SetPhysicsDrawMode(int mode) noexcept = 0;

protected:
    ~IState() = default;
};

} // namespace ZHLN::COM

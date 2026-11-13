// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Read-only engine state: values, not mutable references a caller can
// corrupt between frames (CullingStats, engine configuration).

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Profiler.hpp>

namespace ZHLN {

class Engine;

class ZHLN_API EngineState {
  public:
    [[nodiscard]] auto CullingStats() const noexcept -> ZHLN::CullingStats;
    void               ResetCullingStats() noexcept;
    void               SetCullingEnabled(bool enabled) noexcept;
    void               SetFreezeFrustum(bool freeze) noexcept;
    [[nodiscard]] auto Configuration() const noexcept -> EngineConfig;

  private:
    friend class Engine;
    explicit EngineState(Engine& engine) noexcept: _engine(&engine) {
    }
    Engine* _engine;
};

} // namespace ZHLN

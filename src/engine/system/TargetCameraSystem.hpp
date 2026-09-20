// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Entity.hpp>
#include <optional>

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
} // namespace ECS

struct Camera;

class TargetCameraSystem {
  public:
    TargetCameraSystem()                                     = default;
    ~TargetCameraSystem()                                    = default;
    TargetCameraSystem(const TargetCameraSystem&)            = delete;
    TargetCameraSystem(TargetCameraSystem&&)                 = default;
    TargetCameraSystem& operator=(const TargetCameraSystem&) = delete;
    TargetCameraSystem& operator=(TargetCameraSystem&&)      = default;
    void                Update(Engine& engine, float dt, float alpha) noexcept;
    // @p speedQuery reports the tracked entity's configured free-cam base
    // speed; signature is Engine::FreeCamSpeedQuery. Null (the default for
    // direct/test callers) or a nullopt answer keeps the 12 m/s default.
    void Update(ECS::Registry& reg, Camera& cam, float dt, float alpha, std::optional<float> (*speedQuery)(ECS::Registry&, Entity) = nullptr) noexcept;
};

} // namespace ZHLN

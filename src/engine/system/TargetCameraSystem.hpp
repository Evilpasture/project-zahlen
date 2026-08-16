// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
} // namespace ECS

class Camera;

class TargetCameraSystem {
  public:
    TargetCameraSystem()                                     = default;
    ~TargetCameraSystem()                                    = default;
    TargetCameraSystem(const TargetCameraSystem&)            = delete;
    TargetCameraSystem(TargetCameraSystem&&)                 = default;
    TargetCameraSystem& operator=(const TargetCameraSystem&) = delete;
    TargetCameraSystem& operator=(TargetCameraSystem&&)      = default;
    void                Update(Engine& engine, float dt, float alpha) noexcept;
    void                Update(ECS::Registry& reg, Camera& cam, float dt, float alpha) noexcept;
};

} // namespace ZHLN

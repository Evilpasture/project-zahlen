// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

namespace ZHLN {
class Engine;
namespace ECS {
class Registry;
} // namespace ECS

class Camera;
struct Extent2D;
class CameraSystem {
  public:
    void Update(Engine& engine, float dt, float alpha);
    void Update(ECS::Registry& reg, Camera& cam, Extent2D res, float dt, float alpha);
};
} // namespace ZHLN

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
}

struct Extent2D;
class CameraSystem {
  public:
    void Update(Engine& engine, float dt, float alpha);

    // Projects each camera entity's own pose into that entity's matrices: the
    // camera is component state, so there is no engine camera to read.
    void Update(ECS::Registry& reg, Extent2D res, float dt, float alpha);
};
}

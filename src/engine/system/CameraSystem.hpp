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

// A pass, not an object: both entry points are static, so a caller that holds no
// state has nothing to keep alive. (The frame step used to hide an instance in a
// function-local static, which is state smuggled past everything that could see
// it -- see the camera fold for why the camera itself is data now.)
class CameraSystem {
  public:
    static void Update(Engine& engine, float dt, float alpha);

    // Projects each camera entity's own pose into that entity's matrices: the
    // camera is component state, so there is no engine camera to read.
    static void Update(ECS::Registry& reg, Extent2D res, float dt, float alpha);
};
}

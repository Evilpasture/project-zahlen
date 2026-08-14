// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

namespace ZHLN {
class Engine;
class CameraSystem {
  public:
    void Update(Engine& engine, float dt, float alpha);
};
} // namespace ZHLN

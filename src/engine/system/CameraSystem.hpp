// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>

namespace ZHLN {
class Engine;
class CameraSystem {
  public:
    void Update(Engine& engine, float dt, float alpha);
};
} // namespace ZHLN

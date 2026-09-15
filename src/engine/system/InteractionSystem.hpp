// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;
struct SystemContext;

class ZHLN_API InteractionSystem {
  public:
    InteractionSystem()  = default;
    ~InteractionSystem() = default;

    void Update(SystemContext& ctx, float dt);
};

} // namespace ZHLN

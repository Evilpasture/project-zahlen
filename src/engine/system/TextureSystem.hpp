// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>

namespace ZHLN {

class Engine;

class ZHLN_API TextureSystem {
  public:
    static void Update(Engine& engine, float dt);
    [[nodiscard]] static uint32_t ResolveIndex(Engine& engine, TextureHandle handle) noexcept;
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/UIRenderSystem.hpp
#pragma once
#include <Zahlen/Common.h>

namespace ZHLN {
class Engine;

class ZHLN_API UIRenderSystem {
  public:
	static void Update(Engine& engine);
};
} // namespace ZHLN

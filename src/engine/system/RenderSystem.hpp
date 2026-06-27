// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Render.hpp>
#include <expected>

namespace ZHLN {
class Engine;

class ZHLN_API RenderSystem {
  public:
	// Runs the main scene geometry and light submission pass
	static std::expected<void, RenderFrameResult> Update(Engine& engine);
};
} // namespace ZHLN

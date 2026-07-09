// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN {
class Engine;
struct Camera;

class InputSystem {
  public:
	void Update(Engine& engine);
	void PlayerInputTranslate(Engine& engine, const Camera& cam);
};
} // namespace ZHLN

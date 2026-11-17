// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FreeCam.hpp"
#include <Zahlen/Engine/GameModule.hpp>
namespace ZHLN {
class Engine;
namespace FreeCam {
auto Describe() -> Module;
void Install(Engine&);
void Attach(Engine&, float speed = 12.0f, bool lookRequiresButton = true);
void Detach(Engine&);
void Apply(Engine&, float dt);
} // namespace FreeCam
} // namespace ZHLN

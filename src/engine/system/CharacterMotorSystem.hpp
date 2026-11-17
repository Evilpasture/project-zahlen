// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace ZHLN {
class Engine;
namespace CharacterMotorSystem {
void PreStep(Engine& engine, float dt);
void PostStep(Engine& engine);
} // namespace CharacterMotorSystem
} // namespace ZHLN

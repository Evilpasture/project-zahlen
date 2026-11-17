// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "IK.hpp"
#include "Runtime.hpp"
#include <Zahlen/Engine/NativeEngineFactory.hpp>
namespace ZHLN::Animation {
auto CreateEngine(const COM::ChainInNode* config) noexcept -> COM::Result<COM::IEngine*> {
    return COM::CreateConfiguredEngine(config, +[](Engine& engine) { IK::Install(engine); });
}
} // namespace ZHLN::Animation

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/EngineCode.hpp
#pragma once
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

enum class EngineInitError : uint8_t {
    WindowCreationFailed[[= ZHLN::Reflect::Description("Window creation failed")]] = 1,
    TTYInitializationFailed[[= ZHLN::Reflect::Description("TTY initialization failed")]],
    RenderInitializationFailed[[= ZHLN::Reflect::Description("Render initialization failed")]],
    PhysicsInitializationFailed[[= ZHLN::Reflect::Description("Physics initialization failed")]],
    AudioInitializationFailed[[= ZHLN::Reflect::Description("Audio initialization failed")]],
    AssetInitializationFailed[[= ZHLN::Reflect::Description("Asset initialization failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown engine initialization error")]],
};

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/EngineCode.hpp
#pragma once
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

enum class EngineInitError : uint8_t {
    Success = 0,
    WindowCreationFailed [[= Reflect::Description("Window creation failed")]],
    TTYInitializationFailed [[= Reflect::Description("TTY initialization failed")]],
    RenderInitializationFailed [[= Reflect::Description("Render initialization failed")]],
    PhysicsInitializationFailed [[= Reflect::Description("Physics initialization failed")]],
    AudioInitializationFailed [[= Reflect::Description("Audio initialization failed")]],
    AssetInitializationFailed [[= Reflect::Description("Asset initialization failed")]],
    UnknownError [[= Reflect::Description("Unknown engine initialization error")]],
};

} // namespace ZHLN
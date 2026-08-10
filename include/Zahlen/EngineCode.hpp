// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/EngineCode.hpp
#pragma once
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

enum class EngineInitError : uint8_t {
    Success = 0,
    WindowCreationFailed,
    TTYInitializationFailed,
    RenderInitializationFailed,
    PhysicsInitializationFailed,
    AudioInitializationFailed,
    AssetInitializationFailed,
    UnknownError
};

} // namespace ZHLN

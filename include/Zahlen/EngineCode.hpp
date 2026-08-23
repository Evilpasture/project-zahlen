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
    WindowCreationFailed ZHLN_ANNOTATE(Reflect::Description("Window creation failed")),
    TTYInitializationFailed ZHLN_ANNOTATE(Reflect::Description("TTY initialization failed")),
    RenderInitializationFailed ZHLN_ANNOTATE(Reflect::Description("Render initialization failed")),
    PhysicsInitializationFailed ZHLN_ANNOTATE(Reflect::Description("Physics initialization failed")),
    AudioInitializationFailed ZHLN_ANNOTATE(Reflect::Description("Audio initialization failed")),
    AssetInitializationFailed ZHLN_ANNOTATE(Reflect::Description("Asset initialization failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown engine initialization error")),
};

} // namespace ZHLN
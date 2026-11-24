// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Engine-private error enums, consolidated from the .cpp files that used to
// define them locally (Engine.cpp, KernelSession.cpp, World.cpp,
// system/RenderSystem.cpp) so CreateErrorRegistryRaw can pre-register every
// category the engine itself can return.

#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN {

enum class EngineInitError : uint8_t {
    EngineAllocationFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine instance allocation failed"> {}) = 1,
};

enum class KernelInitError : uint8_t {
    WindowCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"Window creation failed"> {}) = 1,
    TTYInitializationFailed    ZHLN_ANNOTATION(ZHLN::Description<"TTY initialization failed"> {}),
    RenderInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Render initialization failed"> {}),
    KernelAllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"Kernel instance allocation failed"> {}),
};

enum class WorldInitError : uint8_t {
    PhysicsInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Physics initialization failed"> {}) = 1,
    WorldAllocationFailed       ZHLN_ANNOTATION(ZHLN::Description<"World instance allocation failed"> {}),
};

enum class RenderSystemError : uint8_t {
    NoMainCamera ZHLN_ANNOTATION(ZHLN::Description<"The frame has no main camera entity to render the scene from"> {}) = 1,
    EnvironmentImageUnavailable ZHLN_ANNOTATION(ZHLN::Description<"The scene's environment image was not supplied to the asset library"> {}),
    // Appended, not inserted: RenderSystemError is registered (ErrorRegistryAdapter),
    // so an existing member's ordinal is part of what the table publishes.
    RenderContextUnavailable ZHLN_ANNOTATION(ZHLN::Description<"No render context is available: it was never created or has already been torn down"> {}),
};

} // namespace ZHLN

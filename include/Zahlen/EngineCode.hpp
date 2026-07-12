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

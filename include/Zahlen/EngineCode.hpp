// include/Zahlen/EngineCode.hpp
#pragma once
#include <cstdint>
#include <string_view>

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

constexpr auto ToString(EngineInitError error) noexcept -> std::string_view {
    switch (error) {
        case EngineInitError::Success: return "Success";
        case EngineInitError::WindowCreationFailed: return "WindowCreationFailed";
        case EngineInitError::TTYInitializationFailed: return "TTYInitializationFailed";
        case EngineInitError::RenderInitializationFailed: return "RenderInitializationFailed";
        case EngineInitError::PhysicsInitializationFailed: return "PhysicsInitializationFailed";
        case EngineInitError::AudioInitializationFailed: return "AudioInitializationFailed";
        case EngineInitError::AssetInitializationFailed: return "AssetInitializationFailed";
        case EngineInitError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

} // namespace ZHLN

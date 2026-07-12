// include/Zahlen/RenderCode.hpp
#pragma once
#include <cstdint>
#include <string_view>

namespace ZHLN {

enum class RenderInitError : uint8_t {
    Success = 0,
    InstanceCreationFailed,
    SurfaceCreationFailed,
    NoSuitableDeviceFound,
    DeviceCreationFailed,
    SubsystemAllocationFailed,
    PresentationFailed,
    ExtensionQueryFailed,
    ShaderCompilationFailed,
    PipelineLayoutCreationFailed,
    PipelineCreationFailed,
    SamplerCreationFailed,
    UISetupFailed,
    WorkerCommandPoolSetupFailed,
    ParallelRecorderInitializationFailed,
    UnknownError
};

enum class MaterialCreationError : uint8_t {
    Success = 0,
    ShaderCompilationFailed,
    PipelineLayoutCreationFailed,
    PipelineCreationFailed,
    UnknownError
};

enum class ShadowResolutionError : uint8_t { Success = 0, DeviceLost, RecreationFailed, UnknownError };

enum class SurfaceCreationError : uint8_t {
    Success = 0,
    WindowSurfaceUnsupported,
    TTYSurfaceCreationFailed,
    GLFWSurfaceCreationFailed,
    UnknownError
};

enum class ExtensionBuilderError : uint8_t { Success = 0, MissingRequiredExtension, UnknownError };

enum class ShaderStageCreationError : uint8_t {
    Success = 0,
    FileOpenFailed,
    InvalidSpirvSize,
    ShaderLoadingFailed,
    VertexShaderEmpty,
    ShaderModuleCreationFailed,
    UnknownError
};

enum class SamplerCreationError : uint8_t { Success = 0, NullDevice, CreationFailed, UnknownError };

enum class VulkanCallError : uint8_t { Success = 0, VulkanCallFailed, UnknownError };

constexpr auto ToString(RenderInitError error) noexcept -> std::string_view {
    switch (error) {
        case RenderInitError::Success: return "Success";
        case RenderInitError::InstanceCreationFailed: return "InstanceCreationFailed";
        case RenderInitError::SurfaceCreationFailed: return "SurfaceCreationFailed";
        case RenderInitError::NoSuitableDeviceFound: return "NoSuitableDeviceFound";
        case RenderInitError::DeviceCreationFailed: return "DeviceCreationFailed";
        case RenderInitError::SubsystemAllocationFailed: return "SubsystemAllocationFailed";
        case RenderInitError::PresentationFailed: return "PresentationFailed";
        case RenderInitError::ExtensionQueryFailed: return "ExtensionQueryFailed";
        case RenderInitError::ShaderCompilationFailed: return "ShaderCompilationFailed";
        case RenderInitError::PipelineLayoutCreationFailed: return "PipelineLayoutCreationFailed";
        case RenderInitError::PipelineCreationFailed: return "PipelineCreationFailed";
        case RenderInitError::SamplerCreationFailed: return "SamplerCreationFailed";
        case RenderInitError::UISetupFailed: return "UISetupFailed";
        case RenderInitError::WorkerCommandPoolSetupFailed: return "WorkerCommandPoolSetupFailed";
        case RenderInitError::ParallelRecorderInitializationFailed: return "ParallelRecorderInitializationFailed";
        case RenderInitError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(MaterialCreationError error) noexcept -> std::string_view {
    switch (error) {
        case MaterialCreationError::Success: return "Success";
        case MaterialCreationError::ShaderCompilationFailed: return "ShaderCompilationFailed";
        case MaterialCreationError::PipelineLayoutCreationFailed: return "PipelineLayoutCreationFailed";
        case MaterialCreationError::PipelineCreationFailed: return "PipelineCreationFailed";
        case MaterialCreationError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(ShadowResolutionError error) noexcept -> std::string_view {
    switch (error) {
        case ShadowResolutionError::Success: return "Success";
        case ShadowResolutionError::DeviceLost: return "DeviceLost";
        case ShadowResolutionError::RecreationFailed: return "RecreationFailed";
        case ShadowResolutionError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(SurfaceCreationError error) noexcept -> std::string_view {
    switch (error) {
        case SurfaceCreationError::Success: return "Success";
        case SurfaceCreationError::WindowSurfaceUnsupported: return "WindowSurfaceUnsupported";
        case SurfaceCreationError::TTYSurfaceCreationFailed: return "TTYSurfaceCreationFailed";
        case SurfaceCreationError::GLFWSurfaceCreationFailed: return "GLFWSurfaceCreationFailed";
        case SurfaceCreationError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(ExtensionBuilderError error) noexcept -> std::string_view {
    switch (error) {
        case ExtensionBuilderError::Success: return "Success";
        case ExtensionBuilderError::MissingRequiredExtension: return "MissingRequiredExtension";
        case ExtensionBuilderError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(ShaderStageCreationError error) noexcept -> std::string_view {
    switch (error) {
        case ShaderStageCreationError::Success: return "Success";
        case ShaderStageCreationError::FileOpenFailed: return "FileOpenFailed";
        case ShaderStageCreationError::InvalidSpirvSize: return "InvalidSpirvSize";
        case ShaderStageCreationError::ShaderLoadingFailed: return "ShaderLoadingFailed";
        case ShaderStageCreationError::VertexShaderEmpty: return "VertexShaderEmpty";
        case ShaderStageCreationError::ShaderModuleCreationFailed: return "ShaderModuleCreationFailed";
        case ShaderStageCreationError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(SamplerCreationError error) noexcept -> std::string_view {
    switch (error) {
        case SamplerCreationError::Success: return "Success";
        case SamplerCreationError::NullDevice: return "NullDevice";
        case SamplerCreationError::CreationFailed: return "CreationFailed";
        case SamplerCreationError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

constexpr auto ToString(VulkanCallError error) noexcept -> std::string_view {
    switch (error) {
        case VulkanCallError::Success: return "Success";
        case VulkanCallError::VulkanCallFailed: return "VulkanCallFailed";
        case VulkanCallError::UnknownError: return "UnknownError";
    }
    return "UnknownError";
}

} // namespace ZHLN

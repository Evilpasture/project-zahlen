// include/Zahlen/RenderCode.hpp
#pragma once
#include <Zahlen/Error.hpp>
#include <cstdint>

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

enum class MaterialCreationError : uint8_t { Success = 0, ShaderCompilationFailed, PipelineLayoutCreationFailed, PipelineCreationFailed, UnknownError };

enum class ShadowResolutionError : uint8_t { Success = 0, DeviceLost, RecreationFailed, UnknownError };

enum class SurfaceCreationError : uint8_t { Success = 0, WindowSurfaceUnsupported, TTYSurfaceCreationFailed, GLFWSurfaceCreationFailed, UnknownError };

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

} // namespace ZHLN

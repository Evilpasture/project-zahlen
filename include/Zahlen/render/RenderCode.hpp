// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/RenderCode.hpp
#pragma once
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

enum class RenderInitError : uint8_t {
    InstanceCreationFailed[[= ZHLN::Reflect::Description("Vulkan instance creation failed")]] = 1,
    SurfaceCreationFailed[[= ZHLN::Reflect::Description("Window surface creation failed")]],
    NoSuitableDeviceFound[[= ZHLN::Reflect::Description("No suitable Vulkan device found")]],
    DeviceCreationFailed[[= ZHLN::Reflect::Description("Vulkan logical device creation failed")]],
    SubsystemAllocationFailed[[= ZHLN::Reflect::Description("Render subsystem resource allocation failed")]],
    PresentationFailed[[= ZHLN::Reflect::Description("Presentation setup failed")]],
    ExtensionQueryFailed[[= ZHLN::Reflect::Description("Vulkan extension query failed")]],
    ShaderCompilationFailed[[= ZHLN::Reflect::Description("Shader compilation failed")]],
    PipelineLayoutCreationFailed[[= ZHLN::Reflect::Description("Pipeline layout creation failed")]],
    PipelineCreationFailed[[= ZHLN::Reflect::Description("Pipeline creation failed")]],
    SamplerCreationFailed[[= ZHLN::Reflect::Description("Sampler creation failed")]],
    UISetupFailed[[= ZHLN::Reflect::Description("UI setup failed")]],
    WorkerCommandPoolSetupFailed[[= ZHLN::Reflect::Description("Worker command pool setup failed")]],
    ParallelRecorderInitializationFailed[[= ZHLN::Reflect::Description("Parallel command recorder init failed")]],
    OutOfHostMemory[[= ZHLN::Reflect::Description("Out of host memory")]],
    OutOfDeviceMemory[[= ZHLN::Reflect::Description("Out of device memory")]],
    DriverInitializationFailed[[= ZHLN::Reflect::Description("Driver initialization failed")]],
};

enum class BufferCreationError : uint8_t {
    OutOfHostMemory[[= ZHLN::Reflect::Description("Out of host memory")]] = 1,
    OutOfDeviceMemory[[= ZHLN::Reflect::Description("Out of device memory")]],
    InvalidCaptureAddress[[= ZHLN::Reflect::Description("Invalid capture address")]],
    VulkanSubsystemFailure[[= ZHLN::Reflect::Description("Vulkan subsystem failure")]],
};

enum class ImageCreationError : uint8_t {
    OutOfHostMemory[[= ZHLN::Reflect::Description("Out of host memory")]] = 1,
    OutOfDeviceMemory[[= ZHLN::Reflect::Description("Out of device memory")]],
    InvalidCaptureAddress[[= ZHLN::Reflect::Description("Invalid capture address")]],
    VulkanSubsystemFailure[[= ZHLN::Reflect::Description("Vulkan subsystem failure")]],
};

enum class StagingError : uint8_t {
    OutOfHostMemory[[= ZHLN::Reflect::Description("Host memory allocation failed for staging buffer")]] = 1,
    OutOfDeviceMemory[[= ZHLN::Reflect::Description("Device/Host-visible VRAM allocation failed for staging buffer")]],
    MemoryMappingFailed[[= ZHLN::Reflect::Description("Failed to map staging buffer CPU pointer")]],
    InvalidBufferDimensions[[= ZHLN::Reflect::Description("Image upload byte size or dimensions exceed limit")]]
};

enum class MaterialCreationError : uint8_t {
    ShaderCompilationFailed[[= ZHLN::Reflect::Description("Material shader compilation failed")]] = 1,
    PipelineLayoutCreationFailed[[= ZHLN::Reflect::Description("Material pipeline layout creation failed")]],
    PipelineCreationFailed[[= ZHLN::Reflect::Description("Material pipeline creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown material creation error")]],
};

enum class ShadowResolutionError : uint8_t {
    DeviceLost[[= ZHLN::Reflect::Description("Device lost while resizing shadow map")]] = 1,
    RecreationFailed[[= ZHLN::Reflect::Description("Shadow map recreation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown shadow resolution error")]],
};

enum class SurfaceCreationError : uint8_t {
    WindowSurfaceUnsupported[[= ZHLN::Reflect::Description("Window surface unsupported")]] = 1,
    TTYSurfaceCreationFailed[[= ZHLN::Reflect::Description("TTY surface creation failed")]],
    GLFWSurfaceCreationFailed[[= ZHLN::Reflect::Description("GLFW surface creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown surface creation error")]],
};

enum class ExtensionBuilderError : uint8_t {
    MissingRequiredExtension[[= ZHLN::Reflect::Description("A required Vulkan extension is missing")]] = 1,
    UnknownError[[= ZHLN::Reflect::Description("Unknown extension builder error")]],
};

enum class ShaderStageCreationError : uint8_t {
    FileOpenFailed[[= ZHLN::Reflect::Description("Shader file open failed")]] = 1,
    InvalidSpirvSize[[= ZHLN::Reflect::Description("Invalid SPIR-V size")]],
    ShaderLoadingFailed[[= ZHLN::Reflect::Description("Shader loading failed")]],
    VertexShaderEmpty[[= ZHLN::Reflect::Description("Vertex shader is empty")]],
    ShaderModuleCreationFailed[[= ZHLN::Reflect::Description("Shader module creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown shader creation error")]],
};

enum class SamplerCreationError : uint8_t {
    NullDevice[[= ZHLN::Reflect::Description("Null device for sampler creation")]] = 1,
    CreationFailed[[= ZHLN::Reflect::Description("Sampler creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown sampler creation error")]],
};

enum class VulkanCallError : uint8_t {
    VulkanCallFailed[[= ZHLN::Reflect::Description("Vulkan call failed")]] = 1,
    DeviceLost[[= ZHLN::Reflect::Description("Device lost")]],
    FeatureNotPresent[[= ZHLN::Reflect::Description("Required feature not present")]],
};

} // namespace ZHLN

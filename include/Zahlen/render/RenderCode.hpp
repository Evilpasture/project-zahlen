// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/RenderCode.hpp
#pragma once
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

enum class RenderInitError : uint8_t {
    Success = 0,
    InstanceCreationFailed[[= ZHLN::Reflect::Description("Vulkan instance creation failed")]],
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
    UnknownError[[= ZHLN::Reflect::Description("Unknown render initialization error")]],
};

enum class MaterialCreationError : uint8_t {
    Success = 0,
    ShaderCompilationFailed[[= ZHLN::Reflect::Description("Material shader compilation failed")]],
    PipelineLayoutCreationFailed[[= ZHLN::Reflect::Description("Material pipeline layout creation failed")]],
    PipelineCreationFailed[[= ZHLN::Reflect::Description("Material pipeline creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown material creation error")]],
};

enum class ShadowResolutionError : uint8_t {
    Success = 0,
    DeviceLost[[= ZHLN::Reflect::Description("Device lost while resizing shadow map")]],
    RecreationFailed[[= ZHLN::Reflect::Description("Shadow map recreation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown shadow resolution error")]],
};

enum class SurfaceCreationError : uint8_t {
    Success = 0,
    WindowSurfaceUnsupported[[= ZHLN::Reflect::Description("Window surface unsupported")]],
    TTYSurfaceCreationFailed[[= ZHLN::Reflect::Description("TTY surface creation failed")]],
    GLFWSurfaceCreationFailed[[= ZHLN::Reflect::Description("GLFW surface creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown surface creation error")]],
};

enum class ExtensionBuilderError : uint8_t {
    Success = 0,
    MissingRequiredExtension[[= ZHLN::Reflect::Description("A required Vulkan extension is missing")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown extension builder error")]],
};

enum class ShaderStageCreationError : uint8_t {
    Success = 0,
    FileOpenFailed[[= ZHLN::Reflect::Description("Shader file open failed")]],
    InvalidSpirvSize[[= ZHLN::Reflect::Description("Invalid SPIR-V size")]],
    ShaderLoadingFailed[[= ZHLN::Reflect::Description("Shader loading failed")]],
    VertexShaderEmpty[[= ZHLN::Reflect::Description("Vertex shader is empty")]],
    ShaderModuleCreationFailed[[= ZHLN::Reflect::Description("Shader module creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown shader creation error")]],
};

enum class SamplerCreationError : uint8_t {
    Success = 0,
    NullDevice[[= ZHLN::Reflect::Description("Null device for sampler creation")]],
    CreationFailed[[= ZHLN::Reflect::Description("Sampler creation failed")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown sampler creation error")]],
};

enum class VulkanCallError : uint8_t {
    Success = 0,
    VulkanCallFailed[[= ZHLN::Reflect::Description("Vulkan call failed")]],
    FeatureNotPresent[[= ZHLN::Reflect::Description("Required feature not present")]],
    UnknownError[[= ZHLN::Reflect::Description("Unknown Vulkan call error")]],
};

} // namespace ZHLN
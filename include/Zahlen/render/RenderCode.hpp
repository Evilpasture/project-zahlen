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
    InstanceCreationFailed [[= Reflect::Description("Vulkan instance creation failed")]],
    SurfaceCreationFailed [[= Reflect::Description("Window surface creation failed")]],
    NoSuitableDeviceFound [[= Reflect::Description("No suitable Vulkan device found")]],
    DeviceCreationFailed [[= Reflect::Description("Vulkan logical device creation failed")]],
    SubsystemAllocationFailed [[= Reflect::Description("Render subsystem resource allocation failed")]],
    PresentationFailed [[= Reflect::Description("Presentation setup failed")]],
    ExtensionQueryFailed [[= Reflect::Description("Vulkan extension query failed")]],
    ShaderCompilationFailed [[= Reflect::Description("Shader compilation failed")]],
    PipelineLayoutCreationFailed [[= Reflect::Description("Pipeline layout creation failed")]],
    PipelineCreationFailed [[= Reflect::Description("Pipeline creation failed")]],
    SamplerCreationFailed [[= Reflect::Description("Sampler creation failed")]],
    UISetupFailed [[= Reflect::Description("UI setup failed")]],
    WorkerCommandPoolSetupFailed [[= Reflect::Description("Worker command pool setup failed")]],
    ParallelRecorderInitializationFailed [[= Reflect::Description("Parallel command recorder init failed")]],
    UnknownError [[= Reflect::Description("Unknown render initialization error")]],
};

enum class MaterialCreationError : uint8_t {
    Success = 0,
    ShaderCompilationFailed [[= Reflect::Description("Material shader compilation failed")]],
    PipelineLayoutCreationFailed [[= Reflect::Description("Material pipeline layout creation failed")]],
    PipelineCreationFailed [[= Reflect::Description("Material pipeline creation failed")]],
    UnknownError [[= Reflect::Description("Unknown material creation error")]],
};

enum class ShadowResolutionError : uint8_t {
    Success = 0,
    DeviceLost [[= Reflect::Description("Device lost while resizing shadow map")]],
    RecreationFailed [[= Reflect::Description("Shadow map recreation failed")]],
    UnknownError [[= Reflect::Description("Unknown shadow resolution error")]],
};

enum class SurfaceCreationError : uint8_t {
    Success = 0,
    WindowSurfaceUnsupported [[= Reflect::Description("Window surface unsupported")]],
    TTYSurfaceCreationFailed [[= Reflect::Description("TTY surface creation failed")]],
    GLFWSurfaceCreationFailed [[= Reflect::Description("GLFW surface creation failed")]],
    UnknownError [[= Reflect::Description("Unknown surface creation error")]],
};

enum class ExtensionBuilderError : uint8_t {
    Success = 0,
    MissingRequiredExtension [[= Reflect::Description("A required Vulkan extension is missing")]],
    UnknownError [[= Reflect::Description("Unknown extension builder error")]],
};

enum class ShaderStageCreationError : uint8_t {
    Success = 0,
    FileOpenFailed [[= Reflect::Description("Shader file open failed")]],
    InvalidSpirvSize [[= Reflect::Description("Invalid SPIR-V size")]],
    ShaderLoadingFailed [[= Reflect::Description("Shader loading failed")]],
    VertexShaderEmpty [[= Reflect::Description("Vertex shader is empty")]],
    ShaderModuleCreationFailed [[= Reflect::Description("Shader module creation failed")]],
    UnknownError [[= Reflect::Description("Unknown shader creation error")]],
};

enum class SamplerCreationError : uint8_t {
    Success = 0,
    NullDevice [[= Reflect::Description("Null device for sampler creation")]],
    CreationFailed [[= Reflect::Description("Sampler creation failed")]],
    UnknownError [[= Reflect::Description("Unknown sampler creation error")]],
};

enum class VulkanCallError : uint8_t {
    Success = 0,
    VulkanCallFailed [[= Reflect::Description("Vulkan call failed")]],
    FeatureNotPresent [[= Reflect::Description("Required feature not present")]],
    UnknownError [[= Reflect::Description("Unknown Vulkan call error")]],
};

} // namespace ZHLN
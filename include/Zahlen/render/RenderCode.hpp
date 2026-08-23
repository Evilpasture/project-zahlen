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
    InstanceCreationFailed ZHLN_ANNOTATE(Reflect::Description("Vulkan instance creation failed")),
    SurfaceCreationFailed ZHLN_ANNOTATE(Reflect::Description("Window surface creation failed")),
    NoSuitableDeviceFound ZHLN_ANNOTATE(Reflect::Description("No suitable Vulkan device found")),
    DeviceCreationFailed ZHLN_ANNOTATE(Reflect::Description("Vulkan logical device creation failed")),
    SubsystemAllocationFailed ZHLN_ANNOTATE(Reflect::Description("Render subsystem resource allocation failed")),
    PresentationFailed ZHLN_ANNOTATE(Reflect::Description("Presentation setup failed")),
    ExtensionQueryFailed ZHLN_ANNOTATE(Reflect::Description("Vulkan extension query failed")),
    ShaderCompilationFailed ZHLN_ANNOTATE(Reflect::Description("Shader compilation failed")),
    PipelineLayoutCreationFailed ZHLN_ANNOTATE(Reflect::Description("Pipeline layout creation failed")),
    PipelineCreationFailed ZHLN_ANNOTATE(Reflect::Description("Pipeline creation failed")),
    SamplerCreationFailed ZHLN_ANNOTATE(Reflect::Description("Sampler creation failed")),
    UISetupFailed ZHLN_ANNOTATE(Reflect::Description("UI setup failed")),
    WorkerCommandPoolSetupFailed ZHLN_ANNOTATE(Reflect::Description("Worker command pool setup failed")),
    ParallelRecorderInitializationFailed ZHLN_ANNOTATE(Reflect::Description("Parallel command recorder init failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown render initialization error")),
};

enum class MaterialCreationError : uint8_t {
    Success = 0,
    ShaderCompilationFailed ZHLN_ANNOTATE(Reflect::Description("Material shader compilation failed")),
    PipelineLayoutCreationFailed ZHLN_ANNOTATE(Reflect::Description("Material pipeline layout creation failed")),
    PipelineCreationFailed ZHLN_ANNOTATE(Reflect::Description("Material pipeline creation failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown material creation error")),
};

enum class ShadowResolutionError : uint8_t {
    Success = 0,
    DeviceLost ZHLN_ANNOTATE(Reflect::Description("Device lost while resizing shadow map")),
    RecreationFailed ZHLN_ANNOTATE(Reflect::Description("Shadow map recreation failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown shadow resolution error")),
};

enum class SurfaceCreationError : uint8_t {
    Success = 0,
    WindowSurfaceUnsupported ZHLN_ANNOTATE(Reflect::Description("Window surface unsupported")),
    TTYSurfaceCreationFailed ZHLN_ANNOTATE(Reflect::Description("TTY surface creation failed")),
    GLFWSurfaceCreationFailed ZHLN_ANNOTATE(Reflect::Description("GLFW surface creation failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown surface creation error")),
};

enum class ExtensionBuilderError : uint8_t {
    Success = 0,
    MissingRequiredExtension ZHLN_ANNOTATE(Reflect::Description("A required Vulkan extension is missing")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown extension builder error")),
};

enum class ShaderStageCreationError : uint8_t {
    Success = 0,
    FileOpenFailed ZHLN_ANNOTATE(Reflect::Description("Shader file open failed")),
    InvalidSpirvSize ZHLN_ANNOTATE(Reflect::Description("Invalid SPIR-V size")),
    ShaderLoadingFailed ZHLN_ANNOTATE(Reflect::Description("Shader loading failed")),
    VertexShaderEmpty ZHLN_ANNOTATE(Reflect::Description("Vertex shader is empty")),
    ShaderModuleCreationFailed ZHLN_ANNOTATE(Reflect::Description("Shader module creation failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown shader creation error")),
};

enum class SamplerCreationError : uint8_t {
    Success = 0,
    NullDevice ZHLN_ANNOTATE(Reflect::Description("Null device for sampler creation")),
    CreationFailed ZHLN_ANNOTATE(Reflect::Description("Sampler creation failed")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown sampler creation error")),
};

enum class VulkanCallError : uint8_t {
    Success = 0,
    VulkanCallFailed ZHLN_ANNOTATE(Reflect::Description("Vulkan call failed")),
    FeatureNotPresent ZHLN_ANNOTATE(Reflect::Description("Required feature not present")),
    UnknownError ZHLN_ANNOTATE(Reflect::Description("Unknown Vulkan call error")),
};

} // namespace ZHLN
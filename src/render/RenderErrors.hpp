// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderErrors.hpp
//
// Renderer-internal error categories shared across the Vulkan RHI translation
// units. These are deliberately lightweight and dependency-free (Reflection +
// Error + <cstdint> only) so they can be pulled in at the very start of the
// Rendering.hpp include chain: early, low-level headers such as CommandPool.hpp
// reference RenderInitError and must see it before the much heavier
// RenderCore.hpp is reached. The public Render.hpp API never exposes these —
// callers only ever observe the type-erased ZHLN::Error wrapper.

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

// ============================================================================
// Render Subsystem Initialization Errors (Tier 2)
// ============================================================================
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

// Raised by low-level Vulkan wrappers (e.g. WaitIdle). Stays inside the RHI
// layer: content/asset code must not branch on it. Backend-neutral,
// optional-feature fallback signals live in RenderFeatureError (Render.hpp).
enum class VulkanCallError : uint8_t {
    VulkanCallFailed[[= ZHLN::Reflect::Description("Vulkan call failed")]] = 1,
    DeviceLost[[= ZHLN::Reflect::Description("Device lost")]],
};

} // namespace ZHLN

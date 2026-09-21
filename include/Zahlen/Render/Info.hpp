// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/Info.hpp
//
// What the renderer reports about itself: capability errors, the presentation
// path it chose, the device class it landed on, and the two aliases its verbs
// answer in. No behaviour here -- this is the vocabulary the rest of Render/
// speaks in.
#pragma once
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <string_view>

namespace ZHLN {

// Renderer capability errors: backend-neutral, so content/asset code can branch on
// "this GPU lacks the optional feature" without knowing about Vulkan.
enum class RenderFeatureError : uint8_t {
    FeatureNotSupported ZHLN_ANNOTATION(ZHLN::Description<"The requested render feature is not supported on this device"> {}) = 1,
};

namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f;
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f;
inline constexpr float FarDepth   = 1000.0f;
} // namespace Shadows

// How finished frames reach a display, chosen once at device creation. Kept distinct
// from "headless": a windowed session with no window-system integration (macOS has no
// native Vulkan WSI) presents through the host-GL blit rather than masquerading as a
// CI run.
enum class PresentationMode : uint8_t {
    // Standard Vulkan WSI: VkSurfaceKHR + VkSwapchainKHR.
    NativeSwapchain,
    // Offscreen Vulkan render target copied out and blitted through the
    // HostBlit plugin's own OpenGL window (macOS).
    HostBlit,
    // No window and no presenter at all: CI / servers / --headless.
    OffscreenOnly,
};

// Physical-device class, mirroring the backend's device-type enum without naming it.
enum class PhysicalDeviceType : uint8_t {
    Other         = 0,
    IntegratedGPU = 1,
    DiscreteGPU   = 2,
    VirtualGPU    = 3,
    CPU           = 4,
};

// Snapshot of renderer identity and optional-feature status. `rendererName` and
// `gpuName` stay valid for the lifetime of the producing RenderContext.
struct RenderInfo {
    std::string_view   rendererName         = {};
    std::string_view   gpuName              = {};
    PhysicalDeviceType deviceType           = PhysicalDeviceType::Other;
    PresentationMode   presentationMode     = PresentationMode::OffscreenOnly;
    bool               meshShadingSupported = false;
    bool               meshShadingActive    = false;
    bool               rayTracingSupported  = false;
};

// A fallible renderer operation with only success and error as outcomes
// (BuildMeshBLAS). The frame verbs return FrameOutcome<T> instead, because they have
// a non-failure to report -- see Zahlen/FrameResult.hpp.
using RenderResult = std::expected<void, ErrorCode>;

} // namespace ZHLN

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <cstdint>
#include <expected>
#include <string_view>

namespace ZHLN {

enum class RenderFeatureError : uint8_t {
    FeatureNotSupported ZHLN_ANNOTATION(ZHLN::Description<"The requested render feature is not supported on this device"> {}) = 1,
};

namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f;
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f;
inline constexpr float FarDepth   = 1000.0f;
}

enum class PresentationMode : uint8_t {
    NativeSwapchain,
    HostBlit,
    OffscreenOnly,
};

enum class PhysicalDeviceType : uint8_t {
    Other         = 0,
    IntegratedGPU = 1,
    DiscreteGPU   = 2,
    VirtualGPU    = 3,
    CPU           = 4,
};

struct RenderInfo {
    std::string_view   rendererName         = {};
    std::string_view   gpuName              = {};
    PhysicalDeviceType deviceType           = PhysicalDeviceType::Other;
    PresentationMode   presentationMode     = PresentationMode::OffscreenOnly;
    PacingPolicy       pacingPolicy         = PacingPolicy::LegacyVBlank;
    bool               meshShadingSupported = false;
    bool               meshShadingActive    = false;
    bool               rayTracingSupported  = false;
};

using RenderResult = std::expected<void, ErrorCode>;

}

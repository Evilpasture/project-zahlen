// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Render/Info.hpp>
#include <array>

namespace ZHLN::COM {
// Copied diagnostic strings; no borrowed std::string_view crosses COM.
struct GraphicsDeviceInfo {
    String64           rendererName;
    String256          gpuName;
    PhysicalDeviceType deviceType           = PhysicalDeviceType::Other;
    PresentationMode   presentationMode     = PresentationMode::OffscreenOnly;
    PacingPolicy       pacingPolicy         = PacingPolicy::LegacyVBlank;
    bool               meshShadingSupported = false;
    bool               meshShadingActive    = false;
    bool               rayTracingSupported  = false;
};
static_assert(std::is_standard_layout_v<GraphicsDeviceInfo> && std::is_trivially_copyable_v<GraphicsDeviceInfo>);

struct TextureCubeCreateInfo {
    Span<const std::byte> faces[6] {};
    uint32_t faceSize = 0;
};
struct RenderTextureCreateInfo {
    Extent2D extent {};
    bool hdr = false;
};
struct EnvironmentRadianceInfo {
    Span<const float> rgba {};
    Span<const float> lightingRgba {};
    Span<const std::array<float, 3>> diffuseSH {};
    Extent2D extent {};
    uint64_t contentHash = 0;
    bool renderSkybox = false;
};
struct CaptureInfo {
    Extent2D extent {};
    PixelFormat format = PixelFormat::Rgba8;
    FrameCaptureMetrics metrics {};
    uint64_t byteCount = 0;
};
static_assert(std::is_standard_layout_v<TextureCubeCreateInfo> && std::is_trivially_copyable_v<TextureCubeCreateInfo>);
static_assert(std::is_standard_layout_v<RenderTextureCreateInfo> && std::is_trivially_copyable_v<RenderTextureCreateInfo>);
static_assert(std::is_standard_layout_v<EnvironmentRadianceInfo> && std::is_trivially_copyable_v<EnvironmentRadianceInfo>);
static_assert(std::is_standard_layout_v<CaptureInfo> && std::is_trivially_copyable_v<CaptureInfo>);
}

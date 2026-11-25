// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable COM capability interface exposed by libzahlen_plugin_svg
// alongside COM::IAppLayer. Hosts include only this header to rasterize SVG
// assets at runtime without linking zahlen_svg or resvg.

#include "SVGTypes.hpp"
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::SVG {

// Optional COM::InChain payload accepted by libzahlen_plugin_svg's
// CreateAppLayerRaw entry point.
struct PluginConfig {
    float           dpi             = 96.0f;
    float           defaultFontSize = 12.0f;
    COM::StringView defaultFontFamily {};
    COM::StringView serifFamily {};
    COM::StringView sansSerifFamily {};
    COM::StringView cursiveFamily {};
    COM::StringView fantasyFamily {};
    COM::StringView monospaceFamily {};
    COM::StringView languages {};
    COM::StringView resourcesDir {};
    ShapeRendering  shapeRendering  = ShapeRendering::GeometricPrecision;
    TextRendering   textRendering   = TextRendering::OptimizeLegibility;
    ImageRendering  imageRendering  = ImageRendering::OptimizeQuality;
    bool            loadSystemFonts = false;
};

static_assert(std::is_standard_layout_v<PluginConfig> && std::is_trivially_copyable_v<PluginConfig>);

struct ISvgRasterizer : COM::IObject {
    [[nodiscard]] virtual COM::StringView LibraryVersion() const noexcept = 0;

    [[nodiscard]] virtual COM::Result<Size> MeasureData(
        COM::Span<const uint8_t> svg
    ) const noexcept = 0;

    [[nodiscard]] virtual COM::Result<Size> MeasureFile(
        COM::StringView path
    ) const noexcept = 0;

    // Rasterizes into caller-owned `outPixels` (RGBA8888, width * height * 4
    // bytes) and returns the number of bytes written.
    [[nodiscard]] virtual COM::Result<uint64_t> RasterizeData(
        COM::Span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        FitMode                  fit,
        AlphaMode                alpha,
        COM::Span<uint8_t>       outPixels
    ) const noexcept = 0;

    [[nodiscard]] virtual COM::Result<uint64_t> RasterizeFile(
        COM::StringView    path,
        uint32_t           width,
        uint32_t           height,
        FitMode            fit,
        AlphaMode          alpha,
        COM::Span<uint8_t> outPixels
    ) const noexcept = 0;

    // Rasterizes straight-alpha RGBA8888 and uploads directly to `graphics`.
    [[nodiscard]] virtual COM::Result<TextureID> UploadDataTexture(
        COM::IGraphicsDevice*    graphics,
        COM::StringView          name,
        COM::Span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        FitMode                  fit,
        bool                     isSRGB
    ) const noexcept = 0;

    [[nodiscard]] virtual COM::Result<TextureID> UploadFileTexture(
        COM::IGraphicsDevice* graphics,
        COM::StringView       name,
        COM::StringView       path,
        uint32_t              width,
        uint32_t              height,
        FitMode               fit,
        bool                  isSRGB
    ) const noexcept = 0;

  protected:
    ~ISvgRasterizer() = default;
};

static_assert(COM::AuditInterfaceABI<ISvgRasterizer>(), "ISvgRasterizer violates the COM ABI rules.");

} // namespace ZHLN::SVG

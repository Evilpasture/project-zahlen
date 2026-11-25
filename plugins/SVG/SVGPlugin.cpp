// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + SVG::ISvgRasterizer plugin. Seals
// resvg (Rust) behind a runtime .so boundary so hosts can rasterize SVGs or
// upload SVG textures without a compile/link dependency on resvg.

#include <SVG/ISvgRasterizer.hpp>
#include <SVG/SVG.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace {

using namespace ZHLN;

[[nodiscard]] auto CopyView(COM::StringView view) -> std::string {
    if (view.data == nullptr || view.size == 0) {
        return {};
    }
    return std::string(view.data, view.size);
}

[[nodiscard]] auto BuildOptions(const SVG::PluginConfig& cfg) -> SVG::Options {
    SVG::Options opts {};
    opts.dpi               = cfg.dpi;
    opts.defaultFontSize   = cfg.defaultFontSize;
    opts.defaultFontFamily = CopyView(cfg.defaultFontFamily);
    opts.serifFamily       = CopyView(cfg.serifFamily);
    opts.sansSerifFamily   = CopyView(cfg.sansSerifFamily);
    opts.cursiveFamily     = CopyView(cfg.cursiveFamily);
    opts.fantasyFamily     = CopyView(cfg.fantasyFamily);
    opts.monospaceFamily   = CopyView(cfg.monospaceFamily);
    opts.languages         = CopyView(cfg.languages);
    opts.resourcesDir      = CopyView(cfg.resourcesDir);
    opts.shapeRendering    = cfg.shapeRendering;
    opts.textRendering     = cfg.textRendering;
    opts.imageRendering    = cfg.imageRendering;
    opts.loadSystemFonts   = cfg.loadSystemFonts;
    return opts;
}

class SvgPlugin final : public COM::Implements<SvgPlugin, COM::IAppLayer, SVG::ISvgRasterizer> {
  public:
    explicit SvgPlugin(const SVG::Options& options)
        : _rasterizer(options) {}

    // --- COM::IAppLayer ------------------------------------------------------

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {}

    // --- SVG::ISvgRasterizer -------------------------------------------------

    auto LibraryVersion() const noexcept -> COM::StringView override {
        return COM::StringView(SVG::LibraryVersion());
    }

    auto MeasureData(COM::Span<const uint8_t> svg) const noexcept -> COM::Result<SVG::Size> override {
        if (svg.size > 0 && svg.data == nullptr) {
            return COM::Error::NullArgument;
        }
        const auto doc = _rasterizer.LoadData(std::span<const uint8_t>(svg.data, svg.size));
        if (!doc) {
            return doc.error();
        }
        return doc->NativeSize();
    }

    auto MeasureFile(COM::StringView path) const noexcept -> COM::Result<SVG::Size> override {
        if (path.data == nullptr || path.size == 0) {
            return COM::Error::InvalidArgument;
        }
        const auto doc = _rasterizer.LoadFile(std::string_view(path.data, path.size));
        if (!doc) {
            return doc.error();
        }
        return doc->NativeSize();
    }

    auto RasterizeData(
        COM::Span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        SVG::FitMode             fit,
        SVG::AlphaMode           alpha,
        COM::Span<uint8_t>       outPixels
    ) const noexcept -> COM::Result<uint64_t> override {
        if ((svg.size > 0 && svg.data == nullptr) || (outPixels.size > 0 && outPixels.data == nullptr)) {
            return COM::Error::NullArgument;
        }
        const uint64_t requiredBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
        if (width == 0 || height == 0) {
            return SVG::Error::InvalidDimensions;
        }
        if (outPixels.size < requiredBytes || outPixels.data == nullptr) {
            return COM::Error::InvalidArgument;
        }
        const auto raster = _rasterizer.RasterizeData(std::span<const uint8_t>(svg.data, svg.size), width, height, fit, alpha);
        if (!raster) {
            return raster.error();
        }
        if (raster->pixels.size() > outPixels.size) {
            return COM::Error::InvalidArgument;
        }
        std::memcpy(outPixels.data, raster->pixels.data(), raster->pixels.size());
        return static_cast<uint64_t>(raster->pixels.size());
    }

    auto RasterizeFile(
        COM::StringView    path,
        uint32_t           width,
        uint32_t           height,
        SVG::FitMode       fit,
        SVG::AlphaMode     alpha,
        COM::Span<uint8_t> outPixels
    ) const noexcept -> COM::Result<uint64_t> override {
        if (path.data == nullptr || path.size == 0) {
            return COM::Error::InvalidArgument;
        }
        if (width == 0 || height == 0) {
            return SVG::Error::InvalidDimensions;
        }
        const uint64_t requiredBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
        if (outPixels.size < requiredBytes || outPixels.data == nullptr) {
            return COM::Error::InvalidArgument;
        }
        const auto raster = _rasterizer.RasterizeFile(std::string_view(path.data, path.size), width, height, fit, alpha);
        if (!raster) {
            return raster.error();
        }
        if (raster->pixels.size() > outPixels.size) {
            return COM::Error::InvalidArgument;
        }
        std::memcpy(outPixels.data, raster->pixels.data(), raster->pixels.size());
        return static_cast<uint64_t>(raster->pixels.size());
    }

    auto UploadDataTexture(
        COM::IGraphicsDevice*    graphics,
        COM::StringView          name,
        COM::Span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        SVG::FitMode             fit,
        bool                     isSRGB
    ) const noexcept -> COM::Result<TextureID> override {
        if (graphics == nullptr || (svg.size > 0 && svg.data == nullptr)) {
            return COM::Error::NullArgument;
        }
        const auto raster = _rasterizer.RasterizeData(
            std::span<const uint8_t>(svg.data, svg.size), width, height, fit, SVG::AlphaMode::Straight
        );
        if (!raster) {
            return raster.error();
        }
        return graphics->CreateTexture(name, std::as_bytes(std::span(raster->pixels)), Extent2D {raster->width, raster->height}, isSRGB);
    }

    auto UploadFileTexture(
        COM::IGraphicsDevice* graphics,
        COM::StringView       name,
        COM::StringView       path,
        uint32_t              width,
        uint32_t              height,
        SVG::FitMode          fit,
        bool                  isSRGB
    ) const noexcept -> COM::Result<TextureID> override {
        if (graphics == nullptr) {
            return COM::Error::NullArgument;
        }
        if (path.data == nullptr || path.size == 0) {
            return COM::Error::InvalidArgument;
        }
        const auto raster = _rasterizer.RasterizeFile(
            std::string_view(path.data, path.size), width, height, fit, SVG::AlphaMode::Straight
        );
        if (!raster) {
            return raster.error();
        }
        return graphics->CreateTexture(name, std::as_bytes(std::span(raster->pixels)), Extent2D {raster->width, raster->height}, isSRGB);
    }

  private:
    SVG::Rasterizer _rasterizer;
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::SVG::PluginConfig parsed {};
    const auto              checked = DispatchInChain<ZHLN::SVG::PluginConfig>(
        config, [&](const ZHLN::SVG::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) SvgPlugin(BuildOptions(parsed));
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}

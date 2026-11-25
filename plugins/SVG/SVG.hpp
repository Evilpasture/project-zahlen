// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// SVG rasterization: an owning C++ layer over resvg's C API (resvg.h). This is
// a plugin, not core -- turning an .svg into pixels means linking a Rust
// library (resvg/usvg/tiny-skia) that the engine needs nothing from, so the
// whole directory is optional.
//
// Core SVG rasterization is strictly in-memory / VFS-only: it never reads from
// the host OS filesystem directly. Host tools and CLI utilities that want
// convenience disk-path wrappers can opt into <SVG/SVGFile.hpp>.
//
// resvg.h is never included from this header, and nothing that includes this
// header needs resvg's include path: the C types and the C constants both live
// in SVG.cpp, which is also what keeps a resvg upgrade from recompiling the
// world.
//
// Everything returns std::expected<T, ZHLN::ErrorCode> with Error codes; nothing
// throws (the tree builds -fno-exceptions) and nothing aborts.

#include "SVGTypes.hpp"
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::SVG {

// The transform that maps a @p native-sized document onto a @p width x @p height
// pixmap under @p fit. Contain and Cover centre the result, the way CSS
// object-fit does with the default object-position; Stretch does not need to.
[[nodiscard]] auto FitTransform(Size native, uint32_t width, uint32_t height, FitMode fit = FitMode::Contain) noexcept -> Transform;

// A rendered image: tightly packed RGBA8888 rows, always exactly
// width * height * 4 bytes.
struct Raster {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    AlphaMode            alpha  = AlphaMode::Premultiplied;
    std::vector<uint8_t> pixels;

    [[nodiscard]] auto SizeInBytes() const noexcept -> size_t {
        return pixels.size();
    }

    // Out-of-range reads come back transparent rather than off the end.
    [[nodiscard]] auto At(uint32_t x, uint32_t y) const noexcept -> Pixel;

    // In place, and a no-op when the raster is already straight. Alpha 0 keeps
    // its colour as 0: resvg's premultiplied output has no colour left to
    // recover there.
    void ToStraightAlpha() noexcept;

    // The inverse, for a consumer that wants premultiplied pixels.
    void ToPremultipliedAlpha() noexcept;
};

// How to parse. Plain data on purpose: brace-initialise a variation of it per
// call site, copy it, compare it. The expensive part -- the resvg_options
// struct and the font database inside it -- is built from this by Rasterizer.
struct Options {
    // Unit conversion for pt/pc/in/cm/mm. resvg's default.
    float dpi = 96.0f;

    // Used when the document sets no font-size. resvg's default.
    float defaultFontSize = 12.0f;

    // Used when the document sets no font-family. Empty leaves resvg's own
    // default ("Times New Roman") in place.
    std::string defaultFontFamily;

    // CSS generic family aliases, for documents that ask for `sans-serif`
    // rather than a real family. Empty leaves resvg's defaults in place.
    std::string serifFamily;
    std::string sansSerifFamily;
    std::string cursiveFamily;
    std::string fantasyFamily;
    std::string monospaceFamily;

    // Comma-separated, e.g. "en,en-US". Resolves `systemLanguage` conditionals.
    // Empty leaves resvg's default ("en").
    std::string languages;

    ShapeRendering shapeRendering = ShapeRendering::GeometricPrecision;
    TextRendering  textRendering  = TextRendering::OptimizeLegibility;
    ImageRendering imageRendering = ImageRendering::OptimizeQuality;

    // In-memory TTF/OTF font buffers loaded into resvg's font database ahead of
    // parsing (e.g. read from VFS by the caller). An empty buffer marks the
    // Rasterizer invalid with Error::FontLoadFailed.
    std::vector<std::vector<uint8_t>> fontData;
};

// A parsed SVG document: RAII over resvg_render_tree, and the thing that
// renders. Move-only. Not thread-safe -- one Document per thread, or a lock.
class Document {
  public:
    Document() noexcept;
    ~Document();

    Document(const Document&)                    = delete;
    auto operator=(const Document&) -> Document& = delete;
    Document(Document&&) noexcept;
    auto operator=(Document&&) noexcept -> Document&;

    [[nodiscard]] auto IsValid() const noexcept -> bool;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;
    [[nodiscard]] auto NativeSize() const noexcept -> Size;
    [[nodiscard]] auto BoundingBox() const noexcept -> std::optional<Rect>;
    [[nodiscard]] auto NodeExists(std::string_view id) const noexcept -> bool;
    [[nodiscard]] auto NodeBoundingBox(std::string_view id) const noexcept -> std::optional<Rect>;
    [[nodiscard]] auto NodeTransform(std::string_view id) const noexcept -> std::optional<Transform>;

    [[nodiscard]] auto
        Render(uint32_t width, uint32_t height, const Transform& transform = Transform::Identity(), AlphaMode alpha = AlphaMode::Straight) const noexcept
        -> std::expected<Raster, ErrorCode>;

    [[nodiscard]] auto RenderAtScale(float scale, AlphaMode alpha = AlphaMode::Straight) const noexcept -> std::expected<Raster, ErrorCode>;

    [[nodiscard]] auto RenderFitted(uint32_t width, uint32_t height, FitMode fit = FitMode::Contain, AlphaMode alpha = AlphaMode::Straight) const noexcept
        -> std::expected<Raster, ErrorCode>;

    [[nodiscard]] auto RenderNode(
        std::string_view id,
        uint32_t         width,
        uint32_t         height,
        const Transform& transform = Transform::Identity(),
        AlphaMode        alpha     = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, ErrorCode>;

  private:
    friend class Rasterizer;

    explicit Document(void* adoptedTree) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// A built resvg_options: the parse settings plus the in-memory font database.
// Move-only, not thread-safe.
class Rasterizer {
  public:
    explicit Rasterizer(const Options& options = {});
    ~Rasterizer();

    Rasterizer(const Rasterizer&)                    = delete;
    auto operator=(const Rasterizer&) -> Rasterizer& = delete;
    Rasterizer(Rasterizer&&) noexcept;
    auto operator=(Rasterizer&&) noexcept -> Rasterizer&;

    [[nodiscard]] auto IsValid() const noexcept -> bool;
    [[nodiscard]] auto GetOptions() const noexcept -> const Options&;

    // Parses SVG text (or gzip-compressed SVG) already in memory.
    [[nodiscard]] auto LoadData(std::span<const uint8_t> svg) const noexcept -> std::expected<Document, ErrorCode>;

    [[nodiscard]] auto LoadString(std::string_view svgText) const noexcept -> std::expected<Document, ErrorCode>;

    // Parses an SVG strictly from the provided VFS. Never touches the OS filesystem directly.
    [[nodiscard]] auto LoadVirtual(const COM::IAssetLibrary& assets, std::string_view virtualPath) const noexcept
        -> std::expected<Document, ErrorCode>;
    [[nodiscard]] auto LoadVirtual(const FS::VirtualFileSystem& vfs, std::string_view virtualPath) const noexcept
        -> std::expected<Document, ErrorCode>;

    [[nodiscard]] auto RasterizeData(
        std::span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        FitMode                  fit   = FitMode::Contain,
        AlphaMode                alpha = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, ErrorCode>;

    [[nodiscard]] auto RasterizeVirtual(
        const COM::IAssetLibrary& assets,
        std::string_view          virtualPath,
        uint32_t                  width,
        uint32_t                  height,
        FitMode                   fit   = FitMode::Contain,
        AlphaMode                 alpha = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, ErrorCode>;

    [[nodiscard]] auto RasterizeVirtual(
        const FS::VirtualFileSystem& vfs,
        std::string_view             virtualPath,
        uint32_t                     width,
        uint32_t                     height,
        FitMode                      fit   = FitMode::Contain,
        AlphaMode                    alpha = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, ErrorCode>;

  private:
    [[nodiscard]] auto BuildError() const noexcept -> ErrorCode;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// --- One-shot entry points -------------------------------------------------

[[nodiscard]] auto LoadData(std::span<const uint8_t> svg, const Options& options = {}) noexcept -> std::expected<Document, ErrorCode>;
[[nodiscard]] auto LoadString(std::string_view svgText, const Options& options = {}) noexcept -> std::expected<Document, ErrorCode>;
[[nodiscard]] auto LoadVirtual(const COM::IAssetLibrary& assets, std::string_view virtualPath, const Options& options = {}) noexcept
    -> std::expected<Document, ErrorCode>;
[[nodiscard]] auto LoadVirtual(const FS::VirtualFileSystem& vfs, std::string_view virtualPath, const Options& options = {}) noexcept
    -> std::expected<Document, ErrorCode>;

[[nodiscard]] auto RasterizeData(
    std::span<const uint8_t> svg,
    uint32_t                 width,
    uint32_t                 height,
    FitMode                  fit     = FitMode::Contain,
    AlphaMode                alpha   = AlphaMode::Straight,
    const Options&           options = {}
) noexcept -> std::expected<Raster, ErrorCode>;

[[nodiscard]] auto RasterizeVirtual(
    const COM::IAssetLibrary& assets,
    std::string_view          virtualPath,
    uint32_t                  width,
    uint32_t                  height,
    FitMode                   fit     = FitMode::Contain,
    AlphaMode                 alpha   = AlphaMode::Straight,
    const Options&            options = {}
) noexcept -> std::expected<Raster, ErrorCode>;

[[nodiscard]] auto RasterizeVirtual(
    const FS::VirtualFileSystem& vfs,
    std::string_view             virtualPath,
    uint32_t                     width,
    uint32_t                     height,
    FitMode                      fit     = FitMode::Contain,
    AlphaMode                    alpha   = AlphaMode::Straight,
    const Options&               options = {}
) noexcept -> std::expected<Raster, ErrorCode>;

// --- Library ---------------------------------------------------------------

void               EnableLibraryLog() noexcept;
[[nodiscard]] auto LibraryVersion() noexcept -> std::string_view;

} // namespace ZHLN::SVG

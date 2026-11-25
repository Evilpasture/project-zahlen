// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::SVG {

// Hard ceiling on a rendered pixmap, in pixels (16384 x 16384). Not a resvg
// limit -- a guard, because resvg renders into caller-allocated memory and a
// mistyped scale factor would otherwise ask std::vector for terabytes.
inline constexpr uint64_t kMaxRasterPixels = 268435456ULL;

enum class Error : uint8_t {
    NotUTF8 = 1,           // A path, an id or the document text was not valid UTF-8.
    SVGZUnsupported,       // A .svgz document, and this resvg was built without SVGZ decoding.
    FileOpenFailed,        // No such file, or no read permission.
    MalformedGZip,         // A compressed document whose gzip stream is broken.
    ElementsLimitReached,  // Over resvg's 1,000,000-element security limit.
    InvalidSize,           // width/height <= 0, and no viewBox to fall back on.
    ParsingFailed,         // Not an SVG document.
    NodeNotFound,          // No renderable node with that id (or a zero-sized one).
    InvalidDimensions,     // A zero-sized pixmap, which resvg aborts on rather than returning.
    RasterTooLarge,        // The requested pixmap is over kMaxRasterPixels.
    FontLoadFailed,        // A font payload supplied in Options::fontData was empty or invalid.
    OptionsCreationFailed, // resvg_options_create returned NULL, so the Rasterizer has no settings to parse with.
    InvalidDocument,       // A default-constructed or moved-from Document (or Rasterizer) was used.
    Unknown,               // A resvg error code this build of the wrapper has not seen.
};

// Hints for elements whose own `shape-rendering` is `auto`. SVG.cpp maps each
// enumerator onto its resvg counterpart by name, so these carry no numbering
// contract with resvg_shape_rendering.
enum class ShapeRendering : uint8_t { OptimizeSpeed, CrispEdges, GeometricPrecision };

// Hints for elements whose own `text-rendering` is `auto`.
enum class TextRendering : uint8_t { OptimizeSpeed, OptimizeLegibility, GeometricPrecision };

// Hints for elements whose own `image-rendering` is `auto`.
enum class ImageRendering : uint8_t { OptimizeQuality, OptimizeSpeed };

// What the bytes in a Raster mean.
enum class AlphaMode : uint8_t { Premultiplied, Straight };

// How a document's native size is mapped onto a pixmap of a different size.
enum class FitMode : uint8_t {
    Contain, // Whole document visible, aspect kept, letterboxed (CSS object-fit: contain).
    Cover,   // Pixmap fully covered, aspect kept, overflow clipped (CSS object-fit: cover).
    Stretch, // Pixmap fully covered, aspect NOT kept (CSS object-fit: fill).
};

// The document's own width/height, in SVG user units at Options::dpi.
struct Size {
    float width  = 0.0f;
    float height = 0.0f;
};

struct Rect {
    float x      = 0.0f;
    float y      = 0.0f;
    float width  = 0.0f;
    float height = 0.0f;
};

// A row-major 2D affine transform: (x, y) maps to (a*x + c*y + e, b*x + d*y + f).
struct Transform {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float e = 0.0f;
    float f = 0.0f;

    [[nodiscard]] static constexpr auto Identity() noexcept -> Transform {
        return {};
    }

    [[nodiscard]] static constexpr auto Scale(float sx, float sy) noexcept -> Transform {
        return {.a = sx, .b = 0.0f, .c = 0.0f, .d = sy, .e = 0.0f, .f = 0.0f};
    }

    [[nodiscard]] static constexpr auto Scale(float uniform) noexcept -> Transform {
        return Scale(uniform, uniform);
    }

    [[nodiscard]] static constexpr auto Translate(float tx, float ty) noexcept -> Transform {
        return {.a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f, .e = tx, .f = ty};
    }
};

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

static_assert(std::is_standard_layout_v<Size> && std::is_trivially_copyable_v<Size>);
static_assert(std::is_standard_layout_v<Rect> && std::is_trivially_copyable_v<Rect>);
static_assert(std::is_standard_layout_v<Transform> && std::is_trivially_copyable_v<Transform>);
static_assert(std::is_standard_layout_v<Pixel> && std::is_trivially_copyable_v<Pixel>);

} // namespace ZHLN::SVG

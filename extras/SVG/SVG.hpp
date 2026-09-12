// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/SVG/SVG.hpp
//
// SVG rasterization: an owning C++ layer over resvg's C API (resvg.h). This is
// an extra, not core -- turning an .svg into pixels means linking a Rust
// library (resvg/usvg/tiny-skia) that the engine needs nothing from, so the
// whole directory is optional. extras/SVG/CMakeLists.txt looks for resvg and,
// when it is not installed, warns and defines no target at all; consumers
// guard on `if(TARGET zahlen_svg)` exactly the way the composition root guards
// on zahlen_editor.
//
// resvg.h is never included from this header, and nothing that includes this
// header needs resvg's include path: the C types and the C constants both live
// in SVG.cpp, which is also what keeps a resvg upgrade from recompiling the
// world.
//
// Everything returns std::expected<T, ZHLN::Error> with SVGError codes; nothing
// throws (the tree builds -fno-exceptions) and nothing aborts -- resvg asserts
// on a NULL options pointer, a NULL tree and a zero-sized pixmap, so this layer
// is what refuses to hand it one.
//
// Three shapes matter:
//
//   Options      plain data: dpi, fonts, resource directory, rendering hints.
//   Rasterizer   Options built into a resvg_options, font database included.
//                Construct one and reuse it -- a system font scan is IO
//                intensive and resvg documents it as once per options object.
//   Document     a parsed SVG tree. Parsing is the expensive half of resvg
//                (XML, layout, text to path); rendering is cheap, so keep the
//                Document when the same asset is rasterized at several sizes.
//
// The one-shot free functions at the bottom (LoadFile, RasterizeFile, ...) are
// those three steps collapsed into a call, for the common "rasterize this icon
// at 64x64" case.
//
// Alpha: resvg renders PREMULTIPLIED RGBA8888, while every other pixel source
// in this engine (stb_image, and so RenderContext::CreateTexture) delivers
// straight alpha. Raster therefore records which of the two it holds, the
// render calls convert to straight alpha unless told otherwise, and
// Raster::ToPremultipliedAlpha()/ToStraightAlpha() go either way in place.
// Uploading stays the engine's business: this header does not re-expose
// CreateTexture, it only produces a buffer that one can be handed.

#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::SVG {

/// Hard ceiling on a rendered pixmap, in pixels (16384 x 16384). Not a resvg
/// limit -- a guard, because resvg renders into caller-allocated memory and a
/// mistyped scale factor would otherwise ask std::vector for terabytes.
inline constexpr uint64_t kMaxRasterPixels = 268435456ULL;

enum class SVGError : uint8_t {
    NotUTF8 = 1,           ///< A path, an id or the document text was not valid UTF-8.
    SVGZUnsupported,       ///< A .svgz document, and this resvg was built without SVGZ decoding.
    FileOpenFailed,        ///< No such file, or no read permission.
    MalformedGZip,         ///< A compressed document whose gzip stream is broken.
    ElementsLimitReached,  ///< Over resvg's 1,000,000-element security limit.
    InvalidSize,           ///< width/height <= 0, and no viewBox to fall back on.
    ParsingFailed,         ///< Not an SVG document.
    NodeNotFound,          ///< No renderable node with that id (or a zero-sized one).
    InvalidDimensions,     ///< A zero-sized pixmap, which resvg aborts on rather than returning.
    RasterTooLarge,        ///< The requested pixmap is over kMaxRasterPixels.
    FontLoadFailed,        ///< A font file named in Options::fontFiles could not be read.
    OptionsCreationFailed, ///< resvg_options_create returned NULL, so the Rasterizer has no settings to parse with.
    InvalidDocument,       ///< A default-constructed or moved-from Document (or Rasterizer) was used.
    Unknown,               ///< A resvg error code this build of the wrapper has not seen.
};

/// Hints for elements whose own `shape-rendering` is `auto`. SVG.cpp maps each
/// enumerator onto its resvg counterpart by name, so these carry no numbering
/// contract with resvg_shape_rendering.
enum class ShapeRendering : uint8_t { OptimizeSpeed, CrispEdges, GeometricPrecision };

/// Hints for elements whose own `text-rendering` is `auto`.
enum class TextRendering : uint8_t { OptimizeSpeed, OptimizeLegibility, GeometricPrecision };

/// Hints for elements whose own `image-rendering` is `auto`.
enum class ImageRendering : uint8_t { OptimizeQuality, OptimizeSpeed };

/// What the bytes in a Raster mean. See the note on premultiplication above.
enum class AlphaMode : uint8_t { Premultiplied, Straight };

/// How a document's native size is mapped onto a pixmap of a different size.
enum class FitMode : uint8_t {
    Contain, ///< Whole document visible, aspect kept, letterboxed (CSS object-fit: contain).
    Cover,   ///< Pixmap fully covered, aspect kept, overflow clipped (CSS object-fit: cover).
    Stretch, ///< Pixmap fully covered, aspect NOT kept (CSS object-fit: fill).
};

/// The document's own width/height, in SVG user units at Options::dpi.
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

/// A row-major 2D affine transform: (x, y) maps to (a*x + c*y + e, b*x + d*y + f).
/// These are resvg's own six numbers (tiny-skia's Transform::from_row order),
/// which is why the render calls take one instead of a scale and an offset.
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

/// The transform that maps a @p native-sized document onto a @p width x @p height
/// pixmap under @p fit. Contain and Cover centre the result, the way CSS
/// object-fit does with the default object-position; Stretch does not need to.
[[nodiscard]] auto FitTransform(Size native, uint32_t width, uint32_t height, FitMode fit = FitMode::Contain) noexcept -> Transform;

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

/// A rendered image: tightly packed RGBA8888 rows, always exactly
/// width * height * 4 bytes.
///
/// That is the layout RenderContext::CreateTexture takes, so uploading a raster
/// is a call to the engine's own function with no repacking and no wrapper here:
/// `ctx.CreateTexture(raster.pixels.data(), raster.width, raster.height)`. The
/// render calls already converted to straight alpha unless Premultiplied was
/// asked for, which is what that function expects.
struct Raster {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    AlphaMode            alpha  = AlphaMode::Premultiplied;
    std::vector<uint8_t> pixels;

    [[nodiscard]] auto SizeInBytes() const noexcept -> size_t {
        return pixels.size();
    }

    /// Out-of-range reads come back transparent rather than off the end.
    [[nodiscard]] auto At(uint32_t x, uint32_t y) const noexcept -> Pixel;

    /// In place, and a no-op when the raster is already straight. Alpha 0 keeps
    /// its colour as 0: resvg's premultiplied output has no colour left to
    /// recover there.
    void ToStraightAlpha() noexcept;

    /// The inverse, for a consumer that wants premultiplied pixels (a
    /// compositor blending straight from this buffer, say).
    void ToPremultipliedAlpha() noexcept;
};

/// How to parse. Plain data on purpose: brace-initialise a variation of it per
/// call site, copy it, compare it. The expensive part -- the resvg_options
/// struct and the font database inside it -- is built from this by Rasterizer.
struct Options {
    /// Unit conversion for pt/pc/in/cm/mm. resvg's default.
    float dpi = 96.0f;

    /// Used when the document sets no font-size. resvg's default.
    float defaultFontSize = 12.0f;

    /// Used when the document sets no font-family. Empty leaves resvg's own
    /// default ("Times New Roman") in place.
    std::string defaultFontFamily;

    /// CSS generic family aliases, for documents that ask for `sans-serif`
    /// rather than a real family. Empty leaves resvg's defaults in place.
    std::string serifFamily;
    std::string sansSerifFamily;
    std::string cursiveFamily;
    std::string fantasyFamily;
    std::string monospaceFamily;

    /// Comma-separated, e.g. "en,en-US". Resolves `systemLanguage` conditionals.
    /// Empty leaves resvg's default ("en").
    std::string languages;

    /// Directory resvg resolves relative references against -- an
    /// `<image href="art/foo.png">`, a font url(). Empty means "derive it":
    /// file loads point resvg at the file's own directory (which the resvg CLI
    /// does and the resvg C API does not), and loads from memory get no
    /// resource directory at all, since memory has no location.
    std::string resourcesDir;

    ShapeRendering shapeRendering = ShapeRendering::GeometricPrecision;
    TextRendering  textRendering  = TextRendering::OptimizeLegibility;
    ImageRendering imageRendering = ImageRendering::OptimizeQuality;

    /// Walk every system font directory and load what is there. Very IO
    /// intensive, and resvg documents it as once per options object -- which is
    /// what Rasterizer's reuse is for. Off by default: a document with no text
    /// does not need it, and a document that does usually wants one explicit
    /// font file rather than the whole system.
    bool loadSystemFonts = false;

    /// Font files loaded into the database ahead of parsing. One that cannot be
    /// read fails the Rasterizer -- IsValid() is false and the first load
    /// through it returns SVGError::FontLoadFailed -- rather than silently
    /// rendering that text in a fallback face.
    std::vector<std::string> fontFiles;
};

/// A parsed SVG document: RAII over resvg_render_tree, and the thing that
/// renders. Move-only. Not thread-safe -- one Document per thread, or a lock.
///
/// A Document owns everything it needs to render: it outlives the Rasterizer
/// and the Options it was parsed from, so a Rasterizer can be a local in a load
/// function while the Documents it produced stay in a cache.
class Document {
  public:
    /// Declared here and defined in SVG.cpp, because the defaulted body needs
    /// Impl complete to instantiate unique_ptr's deleter.
    Document() noexcept;
    ~Document();

    Document(const Document&)                    = delete;
    auto operator=(const Document&) -> Document& = delete;
    Document(Document&&) noexcept;
    auto operator=(Document&&) noexcept -> Document&;

    /// False for a default-constructed or moved-from Document, which has no
    /// tree and cannot render.
    [[nodiscard]] auto IsValid() const noexcept -> bool;

    /// True when there is nothing to draw: an invalid Document, or one that
    /// parsed with no renderable nodes, which renders as a fully transparent
    /// raster rather than as an error.
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;

    /// The document's width/height attributes at Options::dpi. Content outside
    /// the viewBox is clipped at this size; BoundingBox() is the uncropped one.
    [[nodiscard]] auto NativeSize() const noexcept -> Size;

    /// The maximum extent of the document's content, which can be larger or
    /// smaller than NativeSize(). Disengaged when there is nothing in the tree.
    [[nodiscard]] auto BoundingBox() const noexcept -> std::optional<Rect>;

    [[nodiscard]] auto NodeExists(std::string_view id) const noexcept -> bool;

    /// The node's absolute bounding box in canvas coordinates: geometry only,
    /// without stroke or filter region. Disengaged when no renderable node has
    /// that id.
    [[nodiscard]] auto NodeBoundingBox(std::string_view id) const noexcept -> std::optional<Rect>;

    /// The node's accumulated transform in canvas coordinates, for placing a
    /// single icon out of a sheet at its authored position.
    [[nodiscard]] auto NodeTransform(std::string_view id) const noexcept -> std::optional<Transform>;

    /// Renders into a @p width x @p height pixmap through @p transform. The
    /// pixmap is zeroed first: resvg composites onto whatever memory it is
    /// given rather than clearing it.
    [[nodiscard]] auto
        Render(uint32_t width, uint32_t height, const Transform& transform = Transform::Identity(), AlphaMode alpha = AlphaMode::Straight) const noexcept
        -> std::expected<Raster, Error>;

    /// Render at the document's native size times @p scale, which is how an
    /// icon set is produced at 1x/2x/4x without naming a pixel size.
    [[nodiscard]] auto RenderAtScale(float scale, AlphaMode alpha = AlphaMode::Straight) const noexcept -> std::expected<Raster, Error>;

    /// Render into exactly @p width x @p height under @p fit.
    [[nodiscard]] auto RenderFitted(uint32_t width, uint32_t height, FitMode fit = FitMode::Contain, AlphaMode alpha = AlphaMode::Straight) const noexcept
        -> std::expected<Raster, Error>;

    /// Renders only the node with @p id, cropped to itself rather than placed
    /// where the document has it: resvg puts the node's own bounding box origin
    /// at @p transform's translation, so a pixmap the size of
    /// NodeBoundingBox(@p id) captures the node exactly and nothing else. That
    /// is what makes this the "one icon out of a sheet" call.
    ///
    /// To draw the node at its authored position instead, translate by that
    /// box's origin -- RenderNode(id, w, h, Transform::Translate(box->x, box->y)).
    /// The recipe is exact for anything unfiltered; resvg's C API reports a
    /// node's geometry box and its stroke box but not the filter-inclusive box
    /// it crops by, so a filtered node would come out offset by the difference.
    ///
    /// Fails with SVGError::NodeNotFound when the id is absent, not renderable,
    /// or has a zero bounding box.
    [[nodiscard]] auto RenderNode(
        std::string_view id,
        uint32_t         width,
        uint32_t         height,
        const Transform& transform = Transform::Identity(),
        AlphaMode        alpha     = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, Error>;

  private:
    friend class Rasterizer;

    /// Adopts a resvg_render_tree*. Not part of the API: Rasterizer is the only
    /// thing in the engine that has seen resvg.h, and the pointer is void so
    /// that this header does not have to name the type.
    explicit Document(void* adoptedTree) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

/// A built resvg_options: the parse settings plus the font database. Build one
/// per Options and parse many documents through it -- that reuse is the whole
/// point of the type, since resvg_options_load_system_fonts rescans the system
/// font directories on every call.
///
/// Move-only, not thread-safe.
class Rasterizer {
  public:
    explicit Rasterizer(const Options& options = {});
    ~Rasterizer();

    Rasterizer(const Rasterizer&)                    = delete;
    auto operator=(const Rasterizer&) -> Rasterizer& = delete;
    Rasterizer(Rasterizer&&) noexcept;
    auto operator=(Rasterizer&&) noexcept -> Rasterizer&;

    /// False when the native options could not be built (resvg_options_create
    /// returned NULL, or a font file could not be read), or after a move. Loads
    /// through an invalid Rasterizer fail with the reason it is invalid rather
    /// than handing resvg the NULL it asserts on.
    [[nodiscard]] auto IsValid() const noexcept -> bool;

    /// The settings this Rasterizer was built from.
    [[nodiscard]] auto GetOptions() const noexcept -> const Options&;

    /// Parses an .svg (or .svgz, when resvg was built with SVGZ support) from
    /// disk. When Options::resourcesDir was left empty, resvg's resource
    /// directory is pointed at @p path's own directory for this parse.
    [[nodiscard]] auto LoadFile(std::string_view path) const noexcept -> std::expected<Document, Error>;

    /// Parses SVG text (or gzip-compressed SVG) already in memory. No resource
    /// directory is derived: memory has no location, so relative references
    /// resolve only if Options::resourcesDir named one.
    [[nodiscard]] auto LoadData(std::span<const uint8_t> svg) const noexcept -> std::expected<Document, Error>;

    [[nodiscard]] auto LoadString(std::string_view svgText) const noexcept -> std::expected<Document, Error>;

    /// Load and render in one call, into exactly @p width x @p height.
    [[nodiscard]] auto RasterizeFile(
        std::string_view path,
        uint32_t         width,
        uint32_t         height,
        FitMode          fit   = FitMode::Contain,
        AlphaMode        alpha = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, Error>;

    [[nodiscard]] auto RasterizeData(
        std::span<const uint8_t> svg,
        uint32_t                 width,
        uint32_t                 height,
        FitMode                  fit   = FitMode::Contain,
        AlphaMode                alpha = AlphaMode::Straight
    ) const noexcept -> std::expected<Raster, Error>;

  private:
    /// Why IsValid() is false, for the loads to return.
    [[nodiscard]] auto BuildError() const noexcept -> Error;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// --- One-shot entry points -------------------------------------------------
// Each of these builds a Rasterizer for the call and throws it away, which is
// the right trade when a document is parsed once (a loading screen icon) and
// the wrong one when the same Options parse dozens of documents with
// loadSystemFonts on. Reach for Rasterizer directly in that case.

[[nodiscard]] auto LoadFile(std::string_view path, const Options& options = {}) noexcept -> std::expected<Document, Error>;
[[nodiscard]] auto LoadData(std::span<const uint8_t> svg, const Options& options = {}) noexcept -> std::expected<Document, Error>;
[[nodiscard]] auto LoadString(std::string_view svgText, const Options& options = {}) noexcept -> std::expected<Document, Error>;

[[nodiscard]] auto RasterizeFile(
    std::string_view path,
    uint32_t         width,
    uint32_t         height,
    FitMode          fit     = FitMode::Contain,
    AlphaMode        alpha   = AlphaMode::Straight,
    const Options&   options = {}
) noexcept -> std::expected<Raster, Error>;

[[nodiscard]] auto RasterizeData(
    std::span<const uint8_t> svg,
    uint32_t                 width,
    uint32_t                 height,
    FitMode                  fit     = FitMode::Contain,
    AlphaMode                alpha   = AlphaMode::Straight,
    const Options&           options = {}
) noexcept -> std::expected<Raster, Error>;

// --- Library ---------------------------------------------------------------

/// Routes resvg's own warnings (unresolved references, unsupported features) to
/// stderr. Idempotent, and off by default: resvg_init_log may be called only
/// once per process, so the wrapper does the once.
void EnableLibraryLog() noexcept;

/// The resvg version this binary was compiled against, from RESVG_VERSION.
/// Useful in a bug report next to the engine's own version.
[[nodiscard]] auto LibraryVersion() noexcept -> std::string_view;

} // namespace ZHLN::SVG

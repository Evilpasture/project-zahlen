// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/SVG/SVG.cpp
//
// The resvg API surface this file works against -- parsing, the option setters,
// rendering into caller-allocated memory -- lives here and nowhere else. SVG.hpp
// includes resvg.h for its enumerator values alone and names no resvg type in
// any signature, so the handles below stay behind pimpls.

#include <SVG/SVG.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace ZHLN::SVG {

namespace {

/// Far past any pixmap that could be allocated, and well inside float's exact
/// integer range: the point of the bound is to keep the float-to-uint32 cast in
/// RenderAtScale defined for a nonsense scale factor. The real ceiling on a
/// raster is kMaxRasterPixels, enforced in MakeRaster.
constexpr float kMaxRasterDimension = 1.0e9f;

auto ToNative(const Transform& transform) noexcept -> resvg_transform {
    return resvg_transform {.a = transform.a, .b = transform.b, .c = transform.c, .d = transform.d, .e = transform.e, .f = transform.f};
}

/// resvg's error codes are compared by name and never by value: resvg inserts
/// enumerators into the middle of resvg_error between releases (0.48 added
/// RESVG_ERROR_SVGZ_UNSUPPORTED below NOT_AN_UTF8_STR, renumbering everything
/// after it), and this file is compiled against whatever header is installed. A
/// code from a resvg newer than the one this wrapper was written against lands
/// in Unknown rather than being misread as a neighbour.
auto MapError(int32_t code) noexcept -> Error {
    if (code == RESVG_ERROR_NOT_AN_UTF8_STR) {
        return SVGError::NotUtf8;
    }
    if (code == RESVG_ERROR_FILE_OPEN_FAILED) {
        return SVGError::FileOpenFailed;
    }
    if (code == RESVG_ERROR_MALFORMED_GZIP) {
        return SVGError::MalformedGZip;
    }
    if (code == RESVG_ERROR_ELEMENTS_LIMIT_REACHED) {
        return SVGError::ElementsLimitReached;
    }
    if (code == RESVG_ERROR_INVALID_SIZE) {
        return SVGError::InvalidSize;
    }
    if (code == RESVG_ERROR_PARSING_FAILED) {
        return SVGError::ParsingFailed;
    }
    return SVGError::Unknown;
}

/// Allocates the pixmap resvg renders into. resvg composites onto whatever
/// memory it is handed instead of clearing it -- its C API wraps the caller's
/// buffer in a tiny-skia PixmapMut and draws -- so the buffer is zeroed here,
/// which is also what makes a Contain fit letterbox transparent.
///
/// Both zero dimensions and an over-large pixmap are refused rather than passed
/// down: resvg unwraps the pixmap construction, so a 0 x 0 render aborts the
/// process instead of returning an error.
auto MakeRaster(uint32_t width, uint32_t height) noexcept -> std::expected<Raster, Error> {
    if (width == 0 || height == 0) {
        return std::unexpected(SVGError::InvalidDimensions);
    }

    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixelCount > kMaxRasterPixels) {
        return std::unexpected(SVGError::RasterTooLarge);
    }

    Raster raster;
    raster.width  = width;
    raster.height = height;
    raster.alpha  = AlphaMode::Premultiplied;
    raster.pixels.resize(static_cast<size_t>(pixelCount) * 4, 0);
    return raster;
}

constexpr auto UnpremultiplyChannel(uint32_t channel, uint32_t alpha) noexcept -> uint8_t {
    const uint32_t scaled = (channel * 255 + alpha / 2) / alpha;
    return static_cast<uint8_t>(scaled > 255 ? 255 : scaled);
}

constexpr auto PremultiplyChannel(uint32_t channel, uint32_t alpha) noexcept -> uint8_t {
    return static_cast<uint8_t>((channel * alpha + 127) / 255);
}

/// Builds the resvg_options a Rasterizer owns. Returns the handle on success so
/// the caller can hand it to resvg_parse_tree_*; resvg asserts on a NULL
/// options pointer, so a failure here is a failure to construct, never a NULL
/// that reaches the C API.
auto BuildNativeOptions(const Options& settings) noexcept -> std::expected<resvg_options*, Error> {
    resvg_options* native = resvg_options_create();
    if (native == nullptr) {
        return std::unexpected(SVGError::OptionsCreationFailed);
    }

    resvg_options_set_dpi(native, settings.dpi);
    resvg_options_set_font_size(native, settings.defaultFontSize);

    // An empty string is "leave resvg's default alone", not "set the family to
    // the empty name": resvg takes the pointer as UTF-8 and does not treat ""
    // as unset.
    if (!settings.defaultFontFamily.empty()) {
        resvg_options_set_font_family(native, settings.defaultFontFamily.c_str());
    }
    if (!settings.serifFamily.empty()) {
        resvg_options_set_serif_family(native, settings.serifFamily.c_str());
    }
    if (!settings.sansSerifFamily.empty()) {
        resvg_options_set_sans_serif_family(native, settings.sansSerifFamily.c_str());
    }
    if (!settings.cursiveFamily.empty()) {
        resvg_options_set_cursive_family(native, settings.cursiveFamily.c_str());
    }
    if (!settings.fantasyFamily.empty()) {
        resvg_options_set_fantasy_family(native, settings.fantasyFamily.c_str());
    }
    if (!settings.monospaceFamily.empty()) {
        resvg_options_set_monospace_family(native, settings.monospaceFamily.c_str());
    }
    if (!settings.languages.empty()) {
        resvg_options_set_languages(native, settings.languages.c_str());
    }
    if (!settings.resourcesDir.empty()) {
        resvg_options_set_resources_dir(native, settings.resourcesDir.c_str());
    }

    // The hint enums carry resvg's own values (SVG.hpp), so these casts are
    // exact. resvg_error is the one enum they are not used on: resvg renumbers
    // it between releases, which is what MapError below compares by name for.
    resvg_options_set_shape_rendering_mode(native, static_cast<resvg_shape_rendering>(settings.shapeRendering));
    resvg_options_set_text_rendering_mode(native, static_cast<resvg_text_rendering>(settings.textRendering));
    resvg_options_set_image_rendering_mode(native, static_cast<resvg_image_rendering>(settings.imageRendering));

    for (const std::string& fontFile: settings.fontFiles) {
        if (fontFile.empty()) {
            continue;
        }
        if (resvg_options_load_font_file(native, fontFile.c_str()) != RESVG_OK) {
            // Refused rather than logged and ignored: a document that asks for
            // this face would otherwise render in a fallback and look like a
            // layout bug rather than a missing file.
            resvg_options_destroy(native);
            return std::unexpected(SVGError::FontLoadFailed);
        }
    }

    if (settings.loadSystemFonts) {
        resvg_options_load_system_fonts(native);
    }

    return native;
}

} // namespace

// --- Raster ----------------------------------------------------------------

auto Raster::At(uint32_t x, uint32_t y) const noexcept -> Pixel {
    if (x >= width || y >= height) {
        return {};
    }

    const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + x) * 4;
    if (offset + 4 > pixels.size()) {
        return {};
    }

    return Pixel {.r = pixels[offset + 0], .g = pixels[offset + 1], .b = pixels[offset + 2], .a = pixels[offset + 3]};
}

void Raster::ToStraightAlpha() noexcept {
    if (alpha != AlphaMode::Premultiplied) {
        return;
    }

    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t a = pixels[i + 3];
        if (a == 0) {
            // Fully transparent: resvg has already scaled the colour to 0, and
            // there is nothing to divide back out.
            pixels[i + 0] = 0;
            pixels[i + 1] = 0;
            pixels[i + 2] = 0;
            continue;
        }
        if (a == 255) {
            continue;
        }
        pixels[i + 0] = UnpremultiplyChannel(pixels[i + 0], a);
        pixels[i + 1] = UnpremultiplyChannel(pixels[i + 1], a);
        pixels[i + 2] = UnpremultiplyChannel(pixels[i + 2], a);
    }

    alpha = AlphaMode::Straight;
}

void Raster::ToPremultipliedAlpha() noexcept {
    if (alpha != AlphaMode::Straight) {
        return;
    }

    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t a = pixels[i + 3];
        if (a == 255) {
            continue;
        }
        pixels[i + 0] = PremultiplyChannel(pixels[i + 0], a);
        pixels[i + 1] = PremultiplyChannel(pixels[i + 1], a);
        pixels[i + 2] = PremultiplyChannel(pixels[i + 2], a);
    }

    alpha = AlphaMode::Premultiplied;
}

// --- Transform -------------------------------------------------------------

auto FitTransform(Size native, uint32_t width, uint32_t height, FitMode fit) noexcept -> Transform {
    if (width == 0 || height == 0 || native.width <= 0.0f || native.height <= 0.0f) {
        return Transform::Identity();
    }

    const float targetWidth  = static_cast<float>(width);
    const float targetHeight = static_cast<float>(height);
    const float scaleX       = targetWidth / native.width;
    const float scaleY       = targetHeight / native.height;

    if (fit == FitMode::Stretch) {
        return Transform::Scale(scaleX, scaleY);
    }

    // Cover takes the larger scale so the pixmap is filled and the overflow is
    // clipped; Contain takes the smaller so the whole document is visible. Both
    // centre, which is what CSS object-fit does with the default object-position.
    const float uniform = (fit == FitMode::Cover) ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
    const float offsetX = (targetWidth - native.width * uniform) * 0.5f;
    const float offsetY = (targetHeight - native.height * uniform) * 0.5f;

    return Transform {.a = uniform, .b = 0.0f, .c = 0.0f, .d = uniform, .e = offsetX, .f = offsetY};
}

// --- Document --------------------------------------------------------------

struct Document::Impl {
    resvg_render_tree* tree = nullptr;

    Impl() noexcept = default;
    explicit Impl(resvg_render_tree* adopted) noexcept: tree(adopted) {
    }

    ~Impl() {
        // resvg_tree_destroy asserts on NULL, and a moved-from Impl is exactly
        // that.
        if (tree != nullptr) {
            resvg_tree_destroy(tree);
        }
    }

    Impl(const Impl&)                    = delete;
    auto operator=(const Impl&) -> Impl& = delete;
};

Document::Document() noexcept = default;

Document::Document(void* adoptedTree) noexcept: _impl(std::make_unique<Impl>(static_cast<resvg_render_tree*>(adoptedTree))) {
}

Document::~Document() = default;

Document::Document(Document&&) noexcept                    = default;
auto Document::operator=(Document&&) noexcept -> Document& = default;

auto Document::IsValid() const noexcept -> bool {
    return _impl != nullptr && _impl->tree != nullptr;
}

auto Document::IsEmpty() const noexcept -> bool {
    if (!IsValid()) {
        return true;
    }
    return resvg_is_image_empty(_impl->tree);
}

auto Document::NativeSize() const noexcept -> Size {
    if (!IsValid()) {
        return {};
    }
    const resvg_size native = resvg_get_image_size(_impl->tree);
    return Size {.width = native.width, .height = native.height};
}

auto Document::BoundingBox() const noexcept -> std::optional<Rect> {
    if (!IsValid()) {
        return std::nullopt;
    }

    resvg_rect native {};
    if (!resvg_get_image_bbox(_impl->tree, &native)) {
        return std::nullopt;
    }
    return Rect {.x = native.x, .y = native.y, .width = native.width, .height = native.height};
}

auto Document::NodeExists(std::string_view id) const noexcept -> bool {
    if (!IsValid() || id.empty()) {
        return false;
    }
    // resvg takes a NUL-terminated UTF-8 string, so the view is copied. The
    // document text itself is already parsed; this is only the id lookup.
    const std::string idZ(id);
    return resvg_node_exists(_impl->tree, idZ.c_str());
}

auto Document::NodeBoundingBox(std::string_view id) const noexcept -> std::optional<Rect> {
    if (!IsValid() || id.empty()) {
        return std::nullopt;
    }

    const std::string idZ(id);
    resvg_rect        native {};
    if (!resvg_get_node_bbox(_impl->tree, idZ.c_str(), &native)) {
        return std::nullopt;
    }
    return Rect {.x = native.x, .y = native.y, .width = native.width, .height = native.height};
}

auto Document::NodeTransform(std::string_view id) const noexcept -> std::optional<Transform> {
    if (!IsValid() || id.empty()) {
        return std::nullopt;
    }

    const std::string idZ(id);
    resvg_transform   native {};
    if (!resvg_get_node_transform(_impl->tree, idZ.c_str(), &native)) {
        return std::nullopt;
    }
    return Transform {.a = native.a, .b = native.b, .c = native.c, .d = native.d, .e = native.e, .f = native.f};
}

auto Document::Render(uint32_t width, uint32_t height, const Transform& transform, AlphaMode alphaMode) const noexcept -> std::expected<Raster, Error> {
    if (!IsValid()) {
        return std::unexpected(SVGError::InvalidDocument);
    }

    auto raster = MakeRaster(width, height);
    if (!raster.has_value()) {
        return std::unexpected(raster.error());
    }

    resvg_render(_impl->tree, ToNative(transform), width, height, reinterpret_cast<char*>(raster->pixels.data()));

    if (alphaMode == AlphaMode::Straight) {
        raster->ToStraightAlpha();
    }
    return raster;
}

auto Document::RenderAtScale(float scale, AlphaMode alphaMode) const noexcept -> std::expected<Raster, Error> {
    if (!IsValid()) {
        return std::unexpected(SVGError::InvalidDocument);
    }
    if (!std::isfinite(scale) || scale <= 0.0f) {
        return std::unexpected(SVGError::InvalidDimensions);
    }

    const Size  native = NativeSize();
    const float width  = native.width * scale;
    const float height = native.height * scale;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0f || height < 1.0f) {
        return std::unexpected(SVGError::InvalidSize);
    }
    if (width > kMaxRasterDimension || height > kMaxRasterDimension) {
        return std::unexpected(SVGError::RasterTooLarge);
    }

    // Rounded up: a 24x24 icon at 1.5x is 36x36, and truncating to 35x35 would
    // crop a column the caller asked to see. When the rounding adds a pixel the
    // scale transform does not fill, the remainder stays transparent rather
    // than stretching the drawing -- aspect is preserved over filling the box.
    const uint32_t targetWidth  = static_cast<uint32_t>(std::ceil(width));
    const uint32_t targetHeight = static_cast<uint32_t>(std::ceil(height));

    // A bigger pixmap is not a bigger drawing: resvg positions and scales the
    // document with the transform alone, so the scale has to be in it.
    return Render(targetWidth, targetHeight, Transform::Scale(scale), alphaMode);
}

auto Document::RenderFitted(uint32_t width, uint32_t height, FitMode fit, AlphaMode alphaMode) const noexcept -> std::expected<Raster, Error> {
    if (!IsValid()) {
        return std::unexpected(SVGError::InvalidDocument);
    }
    return Render(width, height, FitTransform(NativeSize(), width, height, fit), alphaMode);
}

auto Document::RenderNode(std::string_view id, uint32_t width, uint32_t height, const Transform& transform, AlphaMode alphaMode) const noexcept
    -> std::expected<Raster, Error> {
    if (!IsValid()) {
        return std::unexpected(SVGError::InvalidDocument);
    }
    if (id.empty()) {
        return std::unexpected(SVGError::NodeNotFound);
    }

    auto raster = MakeRaster(width, height);
    if (!raster.has_value()) {
        return std::unexpected(raster.error());
    }

    const std::string idZ(id);
    if (!resvg_render_node(_impl->tree, idZ.c_str(), ToNative(transform), width, height, reinterpret_cast<char*>(raster->pixels.data()))) {
        return std::unexpected(SVGError::NodeNotFound);
    }

    if (alphaMode == AlphaMode::Straight) {
        raster->ToStraightAlpha();
    }
    return raster;
}

// --- Rasterizer ------------------------------------------------------------

struct Rasterizer::Impl {
    Options        settings {};
    resvg_options* native = nullptr;
    // Why native is NULL, when it is: FontLoadFailed or OptionsCreationFailed.
    // A constructor has nowhere to return it, so it waits here for the first
    // load to hand back.
    Error buildError {};

    Impl() noexcept = default;

    ~Impl() {
        // resvg_options_destroy asserts on NULL: a Rasterizer whose options
        // could not be built, or a moved-from one, holds exactly that.
        if (native != nullptr) {
            resvg_options_destroy(native);
        }
    }

    Impl(const Impl&)                    = delete;
    auto operator=(const Impl&) -> Impl& = delete;
};

Rasterizer::Rasterizer(const Options& options): _impl(std::make_unique<Impl>()) {
    _impl->settings = options;

    auto native = BuildNativeOptions(options);
    if (native.has_value()) {
        _impl->native = *native;
        return;
    }

    // The Rasterizer stays constructible so the failure reaches the caller as an
    // error from the load call rather than as an exception nobody can catch.
    _impl->buildError = native.error();
    ZHLN::Log("[SVG] WARNING: resvg options could not be built ({}); loads through this Rasterizer will fail.", native.error());
}

Rasterizer::~Rasterizer() = default;

Rasterizer::Rasterizer(Rasterizer&&) noexcept                    = default;
auto Rasterizer::operator=(Rasterizer&&) noexcept -> Rasterizer& = default;

auto Rasterizer::IsValid() const noexcept -> bool {
    return _impl != nullptr && _impl->native != nullptr;
}

auto Rasterizer::BuildError() const noexcept -> Error {
    if (_impl != nullptr && _impl->buildError) {
        return _impl->buildError;
    }
    return SVGError::OptionsCreationFailed;
}

auto Rasterizer::GetOptions() const noexcept -> const Options& {
    static const Options kNoOptions {};
    if (_impl == nullptr) {
        return kNoOptions;
    }
    return _impl->settings;
}

auto Rasterizer::LoadFile(std::string_view path) const noexcept -> std::expected<Document, Error> {
    if (!IsValid()) {
        return std::unexpected(BuildError());
    }
    if (path.empty()) {
        return std::unexpected(SVGError::FileOpenFailed);
    }

    // resvg wants NUL-terminated UTF-8. A UTF-8 path also works on Windows:
    // Rust converts the str to UTF-16 rather than passing it to the ANSI API.
    const std::string pathZ(path);

    // Relative references -- an <image href="art/foo.png">, a font url() --
    // resolve against resvg's resource directory. The resvg CLI derives it from
    // the file being parsed; the C API does not, so derive it here when the
    // caller did not pin one. Writing it into the native handle is invisible
    // from outside: _impl->settings, which is what GetOptions() returns, keeps
    // saying "derive", and the next load derives again. These methods are const
    // because nothing the caller can observe changes -- a std::unique_ptr's
    // constness does not extend to what it points at.
    if (_impl->settings.resourcesDir.empty()) {
        const std::string parent = std::filesystem::path(pathZ).parent_path().string();
        resvg_options_set_resources_dir(_impl->native, parent.empty() ? nullptr : parent.c_str());
    }

    resvg_render_tree* tree = nullptr;
    const int32_t      code = resvg_parse_tree_from_file(pathZ.c_str(), _impl->native, &tree);
    if (code != RESVG_OK || tree == nullptr) {
        if (tree != nullptr) {
            resvg_tree_destroy(tree);
        }
        return std::unexpected(MapError(code));
    }

    return Document(tree);
}

auto Rasterizer::LoadData(std::span<const uint8_t> svg) const noexcept -> std::expected<Document, Error> {
    if (!IsValid()) {
        return std::unexpected(BuildError());
    }
    if (svg.empty()) {
        // resvg builds a Rust slice from (data, len) with no null check, so an
        // empty buffer is refused here instead of being handed down.
        return std::unexpected(SVGError::ParsingFailed);
    }

    // Undo whatever a previous LoadFile derived: bytes in memory have no
    // location, so a resource directory left over from another file would
    // silently resolve references against the wrong directory.
    if (_impl->settings.resourcesDir.empty()) {
        resvg_options_set_resources_dir(_impl->native, nullptr);
    }

    resvg_render_tree* tree = nullptr;
    const int32_t      code = resvg_parse_tree_from_data(reinterpret_cast<const char*>(svg.data()), svg.size(), _impl->native, &tree);
    if (code != RESVG_OK || tree == nullptr) {
        if (tree != nullptr) {
            resvg_tree_destroy(tree);
        }
        return std::unexpected(MapError(code));
    }

    return Document(tree);
}

auto Rasterizer::LoadString(std::string_view svgText) const noexcept -> std::expected<Document, Error> {
    return LoadData({reinterpret_cast<const uint8_t*>(svgText.data()), svgText.size()});
}

auto Rasterizer::RasterizeFile(std::string_view path, uint32_t width, uint32_t height, FitMode fit, AlphaMode alpha) const noexcept
    -> std::expected<Raster, Error> {
    auto document = LoadFile(path);
    if (!document.has_value()) {
        return std::unexpected(document.error());
    }
    return document->RenderFitted(width, height, fit, alpha);
}

auto Rasterizer::RasterizeData(std::span<const uint8_t> svg, uint32_t width, uint32_t height, FitMode fit, AlphaMode alpha) const noexcept
    -> std::expected<Raster, Error> {
    auto document = LoadData(svg);
    if (!document.has_value()) {
        return std::unexpected(document.error());
    }
    return document->RenderFitted(width, height, fit, alpha);
}

// --- One-shot entry points -------------------------------------------------

auto LoadFile(std::string_view path, const Options& options) noexcept -> std::expected<Document, Error> {
    const Rasterizer rasterizer(options);
    return rasterizer.LoadFile(path);
}

auto LoadData(std::span<const uint8_t> svg, const Options& options) noexcept -> std::expected<Document, Error> {
    const Rasterizer rasterizer(options);
    return rasterizer.LoadData(svg);
}

auto LoadString(std::string_view svgText, const Options& options) noexcept -> std::expected<Document, Error> {
    const Rasterizer rasterizer(options);
    return rasterizer.LoadString(svgText);
}

auto RasterizeFile(std::string_view path, uint32_t width, uint32_t height, FitMode fit, AlphaMode alpha, const Options& options) noexcept
    -> std::expected<Raster, Error> {
    const Rasterizer rasterizer(options);
    return rasterizer.RasterizeFile(path, width, height, fit, alpha);
}

auto RasterizeData(std::span<const uint8_t> svg, uint32_t width, uint32_t height, FitMode fit, AlphaMode alpha, const Options& options) noexcept
    -> std::expected<Raster, Error> {
    const Rasterizer rasterizer(options);
    return rasterizer.RasterizeData(svg, width, height, fit, alpha);
}

// --- Library ---------------------------------------------------------------

void EnableLibraryLog() noexcept {
    // resvg_init_log may be called only once per process, and it is a global
    // setting rather than a per-options one, so the once lives here instead of
    // in every caller.
    [[maybe_unused]] static const bool initialized = []() noexcept {
        resvg_init_log();
        return true;
    }();
}

auto LibraryVersion() noexcept -> std::string_view {
    return RESVG_VERSION;
}

} // namespace ZHLN::SVG

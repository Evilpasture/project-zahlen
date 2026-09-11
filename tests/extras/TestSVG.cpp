// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestSVG.cpp
//
// SVG is an optional layer (extras/SVG) with an optional third-party library
// behind it, so this suite is built only when extras/SVG/CMakeLists.txt found
// resvg and defined zahlen_svg. No resvg, no target, no test: see
// tests/extras/CMakeLists.txt.
//
// Every document here is a string literal or a file written into a temp
// sandbox, so the suite needs no assets, no GPU and no network: nothing in
// SVG.hpp touches the render context, and uploading a raster is a call to the
// engine's own RenderContext::CreateTexture.
//
// The geometry assertions below sample the middle of a region, never an edge,
// and every coordinate in the documents is an integer. That is what keeps them
// independent of how the installed resvg antialiases, which version it is, and
// which of its optional features were enabled when it was built.

#include "TestsFramework.hpp"
#include <SVG/SVG.hpp>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// 32x16 canvas: a blue bar on the left quarter, a red bar across the middle
// half, nothing on the right quarter.
constexpr std::string_view kBars = R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
  <rect x="0" y="0" width="8" height="16" fill="#0000ff"/>
  <rect id="dot" x="8" y="0" width="16" height="16" fill="#ff0000"/>
</svg>)";

// Half-transparent red over nothing. A fully opaque fill cannot tell a
// premultiplied raster from a straight-alpha one -- the two are identical at
// alpha 255 -- so this is the document the alpha tests use.
constexpr std::string_view kTranslucent = R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
  <rect width="16" height="16" fill="#ff0000" fill-opacity="0.5"/>
</svg>)";

// Physical units, so a change of dpi has a measurable effect on the size resvg
// reports.
constexpr std::string_view kInches = R"(<svg xmlns="http://www.w3.org/2000/svg" width="1in" height="0.5in">
  <rect width="1in" height="0.5in" fill="#00ff00"/>
</svg>)";

constexpr std::string_view kNotSvg = "this is not markup of any kind";

auto IsOpaqueRed(const ZHLN::SVG::Pixel& pixel) noexcept -> bool {
    return pixel.a > 240 && pixel.r > 200 && pixel.g < 60 && pixel.b < 60;
}

auto IsOpaqueBlue(const ZHLN::SVG::Pixel& pixel) noexcept -> bool {
    return pixel.a > 240 && pixel.b > 200 && pixel.r < 60 && pixel.g < 60;
}

auto IsTransparent(const ZHLN::SVG::Pixel& pixel) noexcept -> bool {
    return pixel.a == 0;
}

/// The suite's own scratch directory, removed on the way out. tests/helpers has
/// a sandbox of this shape (CookerFixture.hpp's TempSandbox) but it is named and
/// scoped for the cooker, and nothing here needs the rest of that fixture.
struct ScratchDir {
    fs::path root;

    explicit ScratchDir(std::string_view name) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root             = fs::temp_directory_path() / std::format("zhln_svg_{}_{}", name, stamp);
        std::error_code ec;
        fs::create_directories(root, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    ScratchDir(const ScratchDir&)                    = delete;
    auto operator=(const ScratchDir&) -> ScratchDir& = delete;

    [[nodiscard]] auto Write(std::string_view relative, std::string_view contents) const -> fs::path {
        const fs::path  path = root / relative;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        out << contents;
        return path;
    }
};

} // namespace

struct SVGTestSuite {
    enum class SVGTestError : uint8_t {
        ParseFailed  ZHLN_ANNOTATION(ZHLN::Description<"extras/SVG refused to parse a document the suite expected it to accept."> {}) = 1,
        RenderFailed ZHLN_ANNOTATION(ZHLN::Description<"extras/SVG refused to render a document the suite had already parsed."> {}),
    };

    struct Tests {
        // --- 1. The library is reachable and reports itself ---
        std::expected<void, ZHLN::Error> library_version_and_log_are_idempotent() {
            const auto version = ZHLN::SVG::LibraryVersion();
            ZHLN::Test::ExpectFalse(version.empty());
            // RESVG_VERSION is dotted numbers; anything else means the wrapper
            // read the wrong macro.
            ZHLN::Test::ExpectTrue(version.find('.') != std::string_view::npos);
            ZHLN::Println("    [SVG] resvg {}", version);

            // resvg_init_log may only be called once per process, and the
            // wrapper owns that once. Calling it twice must be a no-op, not a
            // second set_logger.
            ZHLN::SVG::EnableLibraryLog();
            ZHLN::SVG::EnableLibraryLog();
            return {};
        }

        // --- 2. Parse and render a document from memory ---
        std::expected<void, ZHLN::Error> rasterizes_a_document_from_memory() {
            auto document = ZHLN::SVG::LoadString(kBars);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            ZHLN::Test::ExpectTrue(document->IsValid());
            ZHLN::Test::ExpectFalse(document->IsEmpty());
            ZHLN::Test::ExpectEq(document->NativeSize().width, 32.0f);
            ZHLN::Test::ExpectEq(document->NativeSize().height, 16.0f);

            // The bounding box is the extent of the CONTENT, not of the canvas:
            // the two bars reach x 0..24 of a 32-wide document, so the right
            // quarter is not in it. This is the difference between NativeSize()
            // and BoundingBox(), and the reason both exist.
            const auto bounds = document->BoundingBox();
            ZHLN::Test::ExpectTrue(bounds.has_value());
            if (bounds) {
                ZHLN::Test::ExpectEq(bounds->x, 0.0f);
                ZHLN::Test::ExpectEq(bounds->width, 24.0f);
                ZHLN::Test::ExpectEq(bounds->height, 16.0f);
            }

            auto raster = document->Render(32, 16);
            if (!ZHLN::Test::ExpectTrue(raster.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }

            ZHLN::Test::ExpectEq(raster->width, 32u);
            ZHLN::Test::ExpectEq(raster->height, 16u);
            ZHLN::Test::ExpectEq(raster->SizeInBytes(), static_cast<size_t>(32 * 16 * 4));
            // Straight alpha is the default, because that is what the rest of
            // the engine hands RenderContext::CreateTexture.
            ZHLN::Test::ExpectTrue(raster->alpha == ZHLN::SVG::AlphaMode::Straight);

            ZHLN::Test::ExpectTrue(IsOpaqueBlue(raster->At(4, 8)));
            ZHLN::Test::ExpectTrue(IsOpaqueRed(raster->At(16, 8)));
            // Nothing is drawn on the right quarter, and resvg composites onto
            // whatever memory it is given: this is transparent only because the
            // wrapper zeroed the pixmap first.
            ZHLN::Test::ExpectTrue(IsTransparent(raster->At(28, 8)));
            return {};
        }

        // --- 3. Fit modes place the drawing where the arithmetic says ---
        std::expected<void, ZHLN::Error> fit_modes_scale_and_centre() {
            auto document = ZHLN::SVG::LoadString(kBars);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            // 32x16 into 32x32, Contain: scale 1, centred vertically, so the
            // bars sit at y 8..24 and the top and bottom are letterboxed.
            auto contained = document->RenderFitted(32, 32, ZHLN::SVG::FitMode::Contain);
            if (ZHLN::Test::ExpectTrue(contained.has_value())) {
                ZHLN::Test::ExpectTrue(IsOpaqueBlue(contained->At(4, 16)));
                ZHLN::Test::ExpectTrue(IsOpaqueRed(contained->At(16, 16)));
                ZHLN::Test::ExpectTrue(IsTransparent(contained->At(16, 3)));
                ZHLN::Test::ExpectTrue(IsTransparent(contained->At(16, 29)));
            }

            // Stretch: scale (2, 2), so blue occupies x 0..16 and red x 16..48.
            auto stretched = document->RenderFitted(64, 32, ZHLN::SVG::FitMode::Stretch);
            if (ZHLN::Test::ExpectTrue(stretched.has_value())) {
                ZHLN::Test::ExpectTrue(IsOpaqueBlue(stretched->At(8, 16)));
                ZHLN::Test::ExpectTrue(IsOpaqueRed(stretched->At(32, 16)));
                ZHLN::Test::ExpectTrue(IsTransparent(stretched->At(56, 16)));
            }

            // Cover: scale 2 (the larger of 1 and 2), centred, so the red bar
            // fills the whole pixmap and the blue bar is cropped off the left.
            auto covered = document->RenderFitted(32, 32, ZHLN::SVG::FitMode::Cover);
            if (ZHLN::Test::ExpectTrue(covered.has_value())) {
                ZHLN::Test::ExpectTrue(IsOpaqueRed(covered->At(1, 1)));
                ZHLN::Test::ExpectTrue(IsOpaqueRed(covered->At(16, 16)));
                ZHLN::Test::ExpectTrue(IsOpaqueRed(covered->At(30, 30)));
            }

            // FitTransform is the same arithmetic without the render, so it can
            // be checked exactly, including the degenerate inputs.
            const auto contain = ZHLN::SVG::FitTransform({32.0f, 16.0f}, 32, 32, ZHLN::SVG::FitMode::Contain);
            ZHLN::Test::ExpectEq(contain.a, 1.0f);
            ZHLN::Test::ExpectEq(contain.d, 1.0f);
            ZHLN::Test::ExpectEq(contain.e, 0.0f);
            ZHLN::Test::ExpectEq(contain.f, 8.0f);

            const auto cover = ZHLN::SVG::FitTransform({32.0f, 16.0f}, 32, 32, ZHLN::SVG::FitMode::Cover);
            ZHLN::Test::ExpectEq(cover.a, 2.0f);
            ZHLN::Test::ExpectEq(cover.e, -16.0f);

            const auto stretch = ZHLN::SVG::FitTransform({32.0f, 16.0f}, 64, 32, ZHLN::SVG::FitMode::Stretch);
            ZHLN::Test::ExpectEq(stretch.a, 2.0f);
            ZHLN::Test::ExpectEq(stretch.d, 2.0f);

            // A zero target or a zero native size has no sensible scale, so the
            // transform is the identity rather than a division by zero.
            ZHLN::Test::ExpectEq(ZHLN::SVG::FitTransform({}, 32, 32).a, 1.0f);
            ZHLN::Test::ExpectEq(ZHLN::SVG::FitTransform({32.0f, 16.0f}, 0, 32).a, 1.0f);
            return {};
        }

        // --- 4. Premultiplied out of resvg, straight out of the wrapper ---
        std::expected<void, ZHLN::Error> alpha_mode_controls_premultiplication() {
            auto document = ZHLN::SVG::LoadString(kTranslucent);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            auto premultiplied = document->Render(16, 16, ZHLN::SVG::Transform::Identity(), ZHLN::SVG::AlphaMode::Premultiplied);
            if (!ZHLN::Test::ExpectTrue(premultiplied.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectTrue(premultiplied->alpha == ZHLN::SVG::AlphaMode::Premultiplied);

            auto straight = document->Render(16, 16);
            if (!ZHLN::Test::ExpectTrue(straight.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectTrue(straight->alpha == ZHLN::SVG::AlphaMode::Straight);

            const auto prePx      = premultiplied->At(8, 8);
            const auto straightPx = straight->At(8, 8);

            // Half-transparent red: alpha 128 either way, colour scaled by it in
            // the premultiplied buffer and not in the straight one. Ranges, not
            // exact values, because where the rounding lands is resvg's business.
            ZHLN::Test::ExpectInRange(static_cast<int>(prePx.a), 120, 136);
            ZHLN::Test::ExpectInRange(static_cast<int>(prePx.r), 105, 145);
            ZHLN::Test::ExpectInRange(static_cast<int>(straightPx.a), 120, 136);
            ZHLN::Test::ExpectInRange(static_cast<int>(straightPx.r), 235, 255);
            ZHLN::Test::ExpectEq(static_cast<int>(straightPx.g), 0);

            // The two conversions are inverses to within the 8-bit rounding.
            premultiplied->ToStraightAlpha();
            ZHLN::Test::ExpectTrue(premultiplied->alpha == ZHLN::SVG::AlphaMode::Straight);
            ZHLN::Test::ExpectInRange(static_cast<int>(premultiplied->At(8, 8).r), static_cast<int>(straightPx.r) - 1, static_cast<int>(straightPx.r) + 1);

            premultiplied->ToPremultipliedAlpha();
            ZHLN::Test::ExpectTrue(premultiplied->alpha == ZHLN::SVG::AlphaMode::Premultiplied);
            ZHLN::Test::ExpectInRange(static_cast<int>(premultiplied->At(8, 8).r), static_cast<int>(prePx.r) - 1, static_cast<int>(prePx.r) + 1);

            // Both are no-ops once the buffer is already in that mode.
            premultiplied->ToPremultipliedAlpha();
            ZHLN::Test::ExpectInRange(static_cast<int>(premultiplied->At(8, 8).r), static_cast<int>(prePx.r) - 1, static_cast<int>(prePx.r) + 1);

            // Reads outside the image are transparent, not out of bounds.
            ZHLN::Test::ExpectTrue(IsTransparent(straight->At(16, 0)));
            ZHLN::Test::ExpectTrue(IsTransparent(straight->At(0, 16)));
            return {};
        }

        // --- 5. Rendering one node by id ---
        std::expected<void, ZHLN::Error> renders_a_single_node_by_id() {
            auto document = ZHLN::SVG::LoadString(kBars);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            ZHLN::Test::ExpectTrue(document->NodeExists("dot"));
            ZHLN::Test::ExpectFalse(document->NodeExists("nope"));
            ZHLN::Test::ExpectFalse(document->NodeExists(""));

            const auto nodeBounds = document->NodeBoundingBox("dot");
            ZHLN::Test::ExpectTrue(nodeBounds.has_value());
            if (nodeBounds) {
                ZHLN::Test::ExpectEq(nodeBounds->x, 8.0f);
                ZHLN::Test::ExpectEq(nodeBounds->width, 16.0f);
            }
            ZHLN::Test::ExpectFalse(document->NodeBoundingBox("nope").has_value());

            const auto nodeTransform = document->NodeTransform("dot");
            ZHLN::Test::ExpectTrue(nodeTransform.has_value());
            ZHLN::Test::ExpectFalse(document->NodeTransform("nope").has_value());

            auto node = document->RenderNode("dot", 32, 16);
            if (!ZHLN::Test::ExpectTrue(node.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectTrue(IsOpaqueRed(node->At(16, 8)));
            // The blue bar is in the document but not in this node, so it must
            // not appear: node rendering is a subset, not the whole canvas.
            ZHLN::Test::ExpectTrue(IsTransparent(node->At(4, 8)));
            ZHLN::Test::ExpectTrue(IsTransparent(node->At(28, 8)));

            const auto missing = document->RenderNode("nope", 32, 16);
            ZHLN::Test::ExpectFalse(missing.has_value());
            if (!missing) {
                ZHLN::Test::ExpectTrue(missing.error().Is(ZHLN::SVG::SVGError::NodeNotFound));
            }
            ZHLN::Test::ExpectFalse(document->RenderNode("", 32, 16).has_value());
            return {};
        }

        // --- 6. Scale, and the inputs that must be refused ---
        std::expected<void, ZHLN::Error> scale_and_refused_inputs() {
            auto document = ZHLN::SVG::LoadString(kBars);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            // 32x16 at 2x is 64x32, and the red bar lands at x 16..48.
            auto doubled = document->RenderAtScale(2.0f);
            if (ZHLN::Test::ExpectTrue(doubled.has_value())) {
                ZHLN::Test::ExpectEq(doubled->width, 64u);
                ZHLN::Test::ExpectEq(doubled->height, 32u);
                ZHLN::Test::ExpectTrue(IsOpaqueRed(doubled->At(32, 16)));
                ZHLN::Test::ExpectTrue(IsTransparent(doubled->At(56, 16)));
            }

            // A fractional scale rounds up rather than cropping a column: 1.5x
            // of 32x16 is 48x24.
            auto fraction = document->RenderAtScale(1.5f);
            if (ZHLN::Test::ExpectTrue(fraction.has_value())) {
                ZHLN::Test::ExpectEq(fraction->width, 48u);
                ZHLN::Test::ExpectEq(fraction->height, 24u);
            }

            // resvg unwraps its pixmap construction, so a zero-sized render
            // would abort the process. The wrapper refuses it instead, which is
            // the assertion that matters.
            const auto zero = document->Render(0, 16);
            ZHLN::Test::ExpectFalse(zero.has_value());
            if (!zero) {
                ZHLN::Test::ExpectTrue(zero.error().Is(ZHLN::SVG::SVGError::InvalidDimensions));
            }
            ZHLN::Test::ExpectFalse(document->Render(16, 0).has_value());
            ZHLN::Test::ExpectFalse(document->RenderAtScale(0.0f).has_value());
            ZHLN::Test::ExpectFalse(document->RenderAtScale(-2.0f).has_value());

            // 20000 x 20000 is 400 million pixels, over kMaxRasterPixels: the
            // allocation is refused before it is attempted.
            const auto huge = document->Render(20000, 20000);
            ZHLN::Test::ExpectFalse(huge.has_value());
            if (!huge) {
                ZHLN::Test::ExpectTrue(huge.error().Is(ZHLN::SVG::SVGError::RasterTooLarge));
            }

            // A scale that cannot be a pixmap size, including NaN and infinity.
            ZHLN::Test::ExpectFalse(document->RenderAtScale(1.0e30f).has_value());
            return {};
        }

        // --- 7. resvg's failures arrive as SVGError, not as an abort ---
        std::expected<void, ZHLN::Error> bad_input_is_an_error_not_a_crash() {
            ZHLN::Test::ExpectFalse(ZHLN::SVG::LoadString(kNotSvg).has_value());
            ZHLN::Test::ExpectFalse(ZHLN::SVG::LoadString("").has_value());

            // resvg builds a Rust slice from (data, length) with no null check,
            // so an empty buffer has to be refused above it.
            const auto empty = ZHLN::SVG::LoadData(std::span<const uint8_t> {});
            ZHLN::Test::ExpectFalse(empty.has_value());
            if (!empty) {
                ZHLN::Test::ExpectTrue(empty.error().Is<ZHLN::SVG::SVGError>());
            }

            const auto missing = ZHLN::SVG::LoadFile("/nonexistent/zhln_svg_no_such_file.svg");
            ZHLN::Test::ExpectFalse(missing.has_value());
            if (!missing) {
                ZHLN::Test::ExpectTrue(missing.error().Is(ZHLN::SVG::SVGError::FileOpenFailed));
            }
            ZHLN::Test::ExpectFalse(ZHLN::SVG::LoadFile("").has_value());

            // Every failure above is an SVGError, which is what makes
            // Error::Message() print something useful about it.
            const auto garbage = ZHLN::SVG::LoadString(kNotSvg);
            if (!garbage) {
                ZHLN::Test::ExpectTrue(garbage.error().Is<ZHLN::SVG::SVGError>());
                ZHLN::Test::ExpectFalse(garbage.error().Message().empty());
                ZHLN::Println("    [SVG] '{}' -> {}", kNotSvg, garbage.error());
            }
            return {};
        }

        // --- 8. Options reach resvg, and a Rasterizer is reusable ---
        std::expected<void, ZHLN::Error> options_and_rasterizer_reuse() {
            // 1in x 0.5in is 96x48 at the default dpi and 192x96 at 192.
            auto at96 = ZHLN::SVG::LoadString(kInches);
            if (!ZHLN::Test::ExpectTrue(at96.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(at96->NativeSize().width, 96.0f);
            ZHLN::Test::ExpectEq(at96->NativeSize().height, 48.0f);

            ZHLN::SVG::Options    hires {.dpi = 192.0f};
            ZHLN::SVG::Rasterizer rasterizer(hires);
            ZHLN::Test::ExpectTrue(rasterizer.IsValid());
            ZHLN::Test::ExpectEq(rasterizer.GetOptions().dpi, 192.0f);

            auto at192 = rasterizer.LoadString(kInches);
            if (!ZHLN::Test::ExpectTrue(at192.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(at192->NativeSize().width, 192.0f);
            ZHLN::Test::ExpectEq(at192->NativeSize().height, 96.0f);

            // One Rasterizer, many documents: that reuse is the reason the type
            // exists, since the font database inside it is expensive to build.
            auto first  = rasterizer.LoadString(kBars);
            auto second = rasterizer.LoadData(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kTranslucent.data()), kTranslucent.size()));
            ZHLN::Test::ExpectTrue(first.has_value());
            ZHLN::Test::ExpectTrue(second.has_value());
            if (first) {
                ZHLN::Test::ExpectEq(first->NativeSize().width, 32.0f);
            }
            if (second) {
                ZHLN::Test::ExpectEq(second->NativeSize().width, 16.0f);
            }

            // A font file that cannot be read is refused rather than ignored, so
            // text never silently falls back to another face: the Rasterizer
            // reports itself invalid and the reason survives to the first load.
            // resvg built without its `text` feature ignores font loading
            // entirely, which is why both branches are acceptable here -- what
            // must not happen is a Rasterizer that claims to be valid and then
            // hands resvg a NULL.
            ZHLN::SVG::Options brokenFonts;
            brokenFonts.fontFiles.emplace_back("/nonexistent/zhln_svg_no_such_font.ttf");
            ZHLN::SVG::Rasterizer withBrokenFonts(brokenFonts);
            if (!withBrokenFonts.IsValid()) {
                const auto refused = withBrokenFonts.LoadString(kBars);
                ZHLN::Test::ExpectFalse(refused.has_value());
                if (!refused) {
                    ZHLN::Test::ExpectTrue(refused.error().Is(ZHLN::SVG::SVGError::FontLoadFailed));
                }
            } else {
                ZHLN::Test::ExpectTrue(withBrokenFonts.LoadString(kBars).has_value());
            }
            return {};
        }

        // --- 9. Ownership: a Document outlives the Rasterizer that parsed it ---
        std::expected<void, ZHLN::Error> documents_are_movable_and_outlive_their_rasterizer() {
            ZHLN::SVG::Document nothing;
            ZHLN::Test::ExpectFalse(nothing.IsValid());
            ZHLN::Test::ExpectTrue(nothing.IsEmpty());
            ZHLN::Test::ExpectFalse(nothing.BoundingBox().has_value());
            ZHLN::Test::ExpectEq(nothing.NativeSize().width, 0.0f);

            const auto invalid = nothing.Render(8, 8);
            ZHLN::Test::ExpectFalse(invalid.has_value());
            if (!invalid) {
                ZHLN::Test::ExpectTrue(invalid.error().Is(ZHLN::SVG::SVGError::InvalidDocument));
            }

            ZHLN::SVG::Document document;
            {
                // The Rasterizer dies at the end of this scope; the Document
                // must not, which is what makes it cacheable.
                ZHLN::SVG::Rasterizer rasterizer;
                auto                  loaded = rasterizer.LoadString(kBars);
                if (!ZHLN::Test::ExpectTrue(loaded.has_value())) {
                    return std::unexpected(SVGTestError::ParseFailed);
                }
                document = std::move(*loaded);
            }

            ZHLN::Test::ExpectTrue(document.IsValid());
            auto afterScope = document.RenderFitted(32, 16);
            if (!ZHLN::Test::ExpectTrue(afterScope.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectTrue(IsOpaqueRed(afterScope->At(16, 8)));

            ZHLN::SVG::Document moved = std::move(document);
            ZHLN::Test::ExpectTrue(moved.IsValid());
            ZHLN::Test::ExpectFalse(document.IsValid());
            ZHLN::Test::ExpectFalse(document.Render(8, 8).has_value());

            // Move assignment over a live document must release the old tree
            // rather than leak it.
            auto replacement = ZHLN::SVG::LoadString(kTranslucent);
            if (ZHLN::Test::ExpectTrue(replacement.has_value())) {
                moved = std::move(*replacement);
                ZHLN::Test::ExpectEq(moved.NativeSize().width, 16.0f);
            }
            return {};
        }

        // --- 10. Files on disk, and the derived resource directory ---
        std::expected<void, ZHLN::Error> loads_a_file_from_disk() {
            const ScratchDir sandbox("file");
            const auto       path = sandbox.Write("art/icon.svg", kBars);

            auto document = ZHLN::SVG::LoadFile(path.string());
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(document->NativeSize().width, 32.0f);

            auto raster = ZHLN::SVG::RasterizeFile(path.string(), 32, 16);
            if (ZHLN::Test::ExpectTrue(raster.has_value())) {
                ZHLN::Test::ExpectTrue(IsOpaqueRed(raster->At(16, 8)));
            }

            // A Rasterizer whose options named no resource directory points
            // resvg at the file's own directory for that parse -- which is what
            // lets an <image href="art/x.png"> resolve -- and then clears it
            // again for a load from memory, which has no location to derive one
            // from. Both halves are observable only as "the parse still works",
            // so what is asserted here is that neither order breaks the next
            // load.
            ZHLN::SVG::Rasterizer rasterizer;
            ZHLN::Test::ExpectTrue(rasterizer.LoadFile(path.string()).has_value());
            ZHLN::Test::ExpectTrue(rasterizer.LoadString(kBars).has_value());
            ZHLN::Test::ExpectTrue(rasterizer.LoadFile(path.string()).has_value());

            // A pinned resource directory is left alone.
            ZHLN::SVG::Options pinned;
            pinned.resourcesDir = (sandbox.root / "elsewhere").string();
            ZHLN::SVG::Rasterizer pinnedRasterizer(pinned);
            ZHLN::Test::ExpectTrue(pinnedRasterizer.LoadFile(path.string()).has_value());
            ZHLN::Test::ExpectEq(pinnedRasterizer.GetOptions().resourcesDir, pinned.resourcesDir);
            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    return ZHLN::Test::Runner::Run<SVGTestSuite>();
}

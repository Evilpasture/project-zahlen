// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestSVG.cpp
//
// SVG is an optional layer (extras/SVG) with an optional third-party library
// behind it, so this suite is built only when extras/SVG/CMakeLists.txt found
// resvg and defined zahlen_svg. No resvg, no target, no test: see
// tests/extras/CMakeLists.txt.
//
// The document under test is a real one rather than a synthetic one: the C++
// logo exactly as Adobe Illustrator 16 exported it, which is the shape a content
// pipeline actually hands this wrapper. It carries an XML declaration, a DOCTYPE
// naming an external DTD, px units, a fractional height (344.35), a viewBox,
// enable-background, xml:space, nested <g>, cubic paths with relative commands
// and implicit repetitions, and polygons. Nothing here is hand-simplified, so a
// parser regression shows up as a parse failure rather than as a slightly
// different rectangle.
//
// Three small documents remain as fixtures for the three things that artwork
// cannot express on its own: exact integer geometry with a deliberately empty
// region (kBars), partial alpha (kTranslucent, because a fully opaque fill is
// byte-identical premultiplied and straight), and physical units (kInches,
// because px is absolute and so cannot show a dpi change).
//
// Every document is a string literal or a file written into a temp sandbox, so
// the suite needs no assets, no GPU and no network.
//
// Colour samples sit tens of user units inside a solid region -- the geometry
// that makes each one safe is written next to it -- and every fill in the
// artwork is a plain hex colour with no gradient, filter, mask or group opacity.
// That is what keeps the assertions independent of how the installed resvg
// antialiases, which version it is, and which of its optional features were
// enabled when it was built.

#include "TestsFramework.hpp"
#include <SVG/SVG.hpp>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// The C++ logo, verbatim from an Illustrator 16 SVG export. Canvas 306 x 344.35,
// four fills: #659AD2 (top facet), #00599C (right facet), #004482 (bottom
// facet), #FFFFFF (the C and the two plus signs).
constexpr std::string_view kLogo = R"svg(<?xml version="1.0" encoding="utf-8"?>
<!-- Generator: Adobe Illustrator 16.0.4, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px"
	 width="306px" height="344.35px" viewBox="0 0 306 344.35" enable-background="new 0 0 306 344.35" xml:space="preserve">
<path fill="#00599C" d="M302.107,258.262c2.401-4.159,3.893-8.845,3.893-13.053V99.14c0-4.208-1.49-8.893-3.892-13.052L153,172.175
	L302.107,258.262z"/>
<path fill="#004482" d="M166.25,341.193l126.5-73.034c3.644-2.104,6.956-5.737,9.357-9.897L153,172.175L3.893,258.263
	c2.401,4.159,5.714,7.793,9.357,9.896l126.5,73.034C147.037,345.401,158.963,345.401,166.25,341.193z"/>
<path fill="#659AD2" d="M302.108,86.087c-2.402-4.16-5.715-7.793-9.358-9.897L166.25,3.156c-7.287-4.208-19.213-4.208-26.5,0
	L13.25,76.19C5.962,80.397,0,90.725,0,99.14v146.069c0,4.208,1.491,8.894,3.893,13.053L153,172.175L302.108,86.087z"/>
<g>
	<path fill="#FFFFFF" d="M153,274.175c-56.243,0-102-45.757-102-102s45.757-102,102-102c36.292,0,70.139,19.53,88.331,50.968
		l-44.143,25.544c-9.105-15.736-26.038-25.512-44.188-25.512c-28.122,0-51,22.878-51,51c0,28.121,22.878,51,51,51
		c18.152,0,35.085-9.776,44.191-25.515l44.143,25.543C223.142,254.644,189.294,274.175,153,274.175z"/>
</g>
<g>
	<polygon fill="#FFFFFF" points="255,166.508 243.666,166.508 243.666,155.175 232.334,155.175 232.334,166.508 221,166.508 
		221,177.841 232.334,177.841 232.334,189.175 243.666,189.175 243.666,177.841 255,177.841 	"/>
</g>
<g>
	<polygon fill="#FFFFFF" points="297.5,166.508 286.166,166.508 286.166,155.175 274.834,155.175 274.834,166.508 263.5,166.508 
		263.5,177.841 274.834,177.841 274.834,189.175 286.166,189.175 286.166,177.841 297.5,177.841 	"/>
</g>
</svg>)svg";

// 32x16 canvas: a blue bar on the left quarter, a red bar across the middle
// half, nothing on the right quarter. Integer coordinates and axis-aligned
// rectangles, so the fit and node tests can assert exact positions and exact
// transparency.
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
// reports. kLogo is sized in px, which is the other half of that rule.
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

/// Compares a sampled pixel against an expected fill. The tolerance is two
/// levels, which is far tighter than any antialiasing at the sample points below
/// and far looser than confusing one of the artwork's four fills with another.
/// On a mismatch it prints what was actually there, because "expected white" is
/// not enough to debug a colour from a coordinate.
auto ExpectPixel(const ZHLN::SVG::Raster& raster, uint32_t x, uint32_t y, int r, int g, int b) -> bool {
    const auto pixel   = raster.At(x, y);
    const auto near    = [](uint8_t actual, int expected) noexcept { return std::abs(static_cast<int>(actual) - expected) <= 2; };
    const bool matches = pixel.a > 240 && near(pixel.r, r) && near(pixel.g, g) && near(pixel.b, b);
    if (!matches) {
        ZHLN::Println("    [SVG] pixel ({}, {}) is ({}, {}, {}, {}), expected ({}, {}, {}, opaque)", x, y, pixel.r, pixel.g, pixel.b, pixel.a, r, g, b);
    }
    return matches;
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

        // --- 2. A real export parses, and its numbers survive as floats ---
        std::expected<void, ZHLN::Error> parses_a_real_world_illustrator_export() {
            auto document = ZHLN::SVG::LoadString(kLogo);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            ZHLN::Test::ExpectTrue(document->IsValid());
            ZHLN::Test::ExpectFalse(document->IsEmpty());

            // width="306px" height="344.35px". px is absolute, and the height is
            // not an integer: a wrapper that rounded the native size here would
            // quietly crop or letterbox every render of the document.
            ZHLN::Test::ExpectEq(document->NativeSize().width, 306.0f);
            ZHLN::Test::ExpectInRange(document->NativeSize().height, 344.34f, 344.36f);

            // This artwork fills its canvas: the hexagon touches x=0 and x=306,
            // and the top facet's curve rises to y=0. So BoundingBox() and
            // NativeSize() agree here -- unlike kBars below, where the content
            // stops three quarters of the way across the canvas. The tolerances
            // are a unit either side because the top of the drawing is a cubic,
            // and where exactly a cubic reaches is resvg's arithmetic.
            const auto bounds = document->BoundingBox();
            ZHLN::Test::ExpectTrue(bounds.has_value());
            if (bounds) {
                ZHLN::Test::ExpectInRange(bounds->x, -0.5f, 0.5f);
                ZHLN::Test::ExpectInRange(bounds->y, -0.5f, 1.0f);
                ZHLN::Test::ExpectInRange(bounds->width, 305.0f, 307.0f);
                ZHLN::Test::ExpectInRange(bounds->height, 343.0f, 345.5f);
            }

            // The same bytes through the span overload, which is how an embedded
            // asset arrives -- an archive entry, a network buffer -- with no path
            // to derive a resource directory from.
            const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(kLogo.data()), kLogo.size());
            auto                           fromData = ZHLN::SVG::LoadData(bytes);
            if (ZHLN::Test::ExpectTrue(fromData.has_value())) {
                ZHLN::Test::ExpectEq(fromData->NativeSize().width, 306.0f);
                ZHLN::Test::ExpectInRange(fromData->NativeSize().height, 344.34f, 344.36f);
            }
            return {};
        }

        // --- 3. Its four fills come back as the four fills it names ---
        std::expected<void, ZHLN::Error> renders_the_artwork_at_its_own_size() {
            auto document = ZHLN::SVG::LoadString(kLogo);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            // The native height is fractional, so "the native size" as a pixmap is
            // 345 rows and the last of them is a sliver resvg only partly covers.
            auto raster = document->Render(306, 345);
            if (!ZHLN::Test::ExpectTrue(raster.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectEq(raster->width, 306u);
            ZHLN::Test::ExpectEq(raster->height, 345u);
            ZHLN::Test::ExpectEq(raster->SizeInBytes(), static_cast<size_t>(306) * 345 * 4);
            // Straight alpha is the default, because that is what the rest of the
            // engine hands RenderContext::CreateTexture.
            ZHLN::Test::ExpectTrue(raster->alpha == ZHLN::SVG::AlphaMode::Straight);

            // Each sample is deep inside one facet, in user units of the 306 x
            // 344.35 canvas:
            //   (153, 30)  #659AD2, 30 below the top curve and 40 above the C.
            //   (153, 96)  #FFFFFF, the middle of the C's band at twelve
            //              o'clock, which runs y 70.175..121.175 there.
            //   (153, 300) #004482, 26 below the C and about 70 inside the
            //              hexagon's two lower edges.
            //   (304, 120) #00599C, on the right facet's straight edge, which is
            //              the line x=306 for y 99.14..245.2.
            ZHLN::Test::ExpectTrue(ExpectPixel(*raster, 153, 30, 101, 154, 210));
            ZHLN::Test::ExpectTrue(ExpectPixel(*raster, 153, 96, 255, 255, 255));
            ZHLN::Test::ExpectTrue(ExpectPixel(*raster, 153, 300, 0, 68, 130));
            ZHLN::Test::ExpectTrue(ExpectPixel(*raster, 304, 120, 0, 89, 156));

            // Asking for premultiplied changes the flag and nothing else, because
            // every pixel sampled is opaque: at alpha 255 the two conventions are
            // the same bytes. The translucent fixture below is what tells them
            // apart.
            auto premultiplied = document->Render(306, 345, ZHLN::SVG::Transform::Identity(), ZHLN::SVG::AlphaMode::Premultiplied);
            if (ZHLN::Test::ExpectTrue(premultiplied.has_value())) {
                ZHLN::Test::ExpectTrue(premultiplied->alpha == ZHLN::SVG::AlphaMode::Premultiplied);
                ZHLN::Test::ExpectEq(premultiplied->SizeInBytes(), raster->SizeInBytes());
                ZHLN::Test::ExpectTrue(ExpectPixel(*premultiplied, 153, 96, 255, 255, 255));
                ZHLN::Test::ExpectTrue(ExpectPixel(*premultiplied, 153, 300, 0, 68, 130));
            }
            return {};
        }

        // --- 4. Fit modes on a document whose aspect is not the target's ---
        std::expected<void, ZHLN::Error> fit_modes_place_a_real_document() {
            auto document = ZHLN::SVG::LoadString(kLogo);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            const auto native = document->NativeSize();

            // Contain into a square: the scale is 100/344.35, the drawing is
            // 88.86 wide, and the 5.57 columns at each side are letterbox. The C
            // lands at device (50, 28) -- its band is 51 units wide, so 15 device
            // pixels, and that sample is in the middle of it.
            auto contained = document->RenderFitted(100, 100, ZHLN::SVG::FitMode::Contain);
            if (ZHLN::Test::ExpectTrue(contained.has_value())) {
                ZHLN::Test::ExpectEq(contained->width, 100u);
                ZHLN::Test::ExpectEq(contained->height, 100u);
                ZHLN::Test::ExpectTrue(IsTransparent(contained->At(1, 50)));
                ZHLN::Test::ExpectTrue(IsTransparent(contained->At(98, 50)));
                ZHLN::Test::ExpectTrue(ExpectPixel(*contained, 50, 28, 255, 255, 255));
                // The centre of the canvas is the hole in the C, which shows the
                // two blue facets through it -- and their seam runs through
                // exactly that point, so what is asserted is opacity, not hue.
                ZHLN::Test::ExpectTrue(contained->At(50, 50).a > 240);
            }

            // Cover into the same square: the scale is 100/306, the drawing is
            // 112.5 tall, so 6.27 rows are cropped at each of the top and bottom
            // and nothing is letterboxed -- content reaches the first column.
            auto covered = document->RenderFitted(100, 100, ZHLN::SVG::FitMode::Cover);
            if (ZHLN::Test::ExpectTrue(covered.has_value())) {
                ZHLN::Test::ExpectEq(covered->width, 100u);
                ZHLN::Test::ExpectEq(covered->height, 100u);
                ZHLN::Test::ExpectTrue(covered->At(0, 50).a > 240);
                ZHLN::Test::ExpectTrue(covered->At(50, 50).a > 240);
            }

            // Stretch into a wide box: both axes scale independently, so the
            // hexagon is squashed and every column and row is covered.
            auto stretched = document->RenderFitted(100, 50, ZHLN::SVG::FitMode::Stretch);
            if (ZHLN::Test::ExpectTrue(stretched.has_value())) {
                ZHLN::Test::ExpectEq(stretched->width, 100u);
                ZHLN::Test::ExpectEq(stretched->height, 50u);
                ZHLN::Test::ExpectTrue(stretched->At(0, 25).a > 240);
                ZHLN::Test::ExpectTrue(stretched->At(50, 25).a > 240);
            }

            // FitTransform is the same arithmetic without the render, so it can be
            // checked as numbers. Ranges rather than equality: the scale is a
            // ratio of 344.35, and asserting its last bits would be asserting the
            // compiler's rounding rather than the wrapper's maths.
            const auto contain = ZHLN::SVG::FitTransform(native, 100, 100, ZHLN::SVG::FitMode::Contain);
            ZHLN::Test::ExpectInRange(contain.a, 0.2903f, 0.2905f);
            ZHLN::Test::ExpectInRange(contain.d, 0.2903f, 0.2905f);
            ZHLN::Test::ExpectEq(contain.b, 0.0f);
            ZHLN::Test::ExpectEq(contain.c, 0.0f);
            ZHLN::Test::ExpectInRange(contain.e, 5.55f, 5.59f); // (100 - 306 * scale) / 2
            ZHLN::Test::ExpectInRange(contain.f, -0.01f, 0.01f);

            const auto cover = ZHLN::SVG::FitTransform(native, 100, 100, ZHLN::SVG::FitMode::Cover);
            ZHLN::Test::ExpectInRange(cover.a, 0.3267f, 0.3269f);
            ZHLN::Test::ExpectInRange(cover.e, -0.01f, 0.01f);
            ZHLN::Test::ExpectInRange(cover.f, -6.28f, -6.25f); // (100 - 344.35 * scale) / 2

            const auto stretch = ZHLN::SVG::FitTransform(native, 100, 50, ZHLN::SVG::FitMode::Stretch);
            ZHLN::Test::ExpectInRange(stretch.a, 0.3267f, 0.3269f);
            ZHLN::Test::ExpectInRange(stretch.d, 0.1451f, 0.1453f);
            ZHLN::Test::ExpectEq(stretch.e, 0.0f);
            ZHLN::Test::ExpectEq(stretch.f, 0.0f);
            return {};
        }

        // --- 5. Scaling rounds up, and a fractional size survives it ---
        std::expected<void, ZHLN::Error> scaling_a_fractional_native_size() {
            auto document = ZHLN::SVG::LoadString(kLogo);
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }

            // 1x of a 344.35-tall document is 345 rows: truncating to 344 would
            // crop a row the artwork really draws into.
            auto one = document->RenderAtScale(1.0f);
            if (ZHLN::Test::ExpectTrue(one.has_value())) {
                ZHLN::Test::ExpectEq(one->width, 306u);
                ZHLN::Test::ExpectEq(one->height, 345u);
            }

            // 2x is 612 x 689 (ceil of 688.7), and every sample doubles with it.
            auto doubled = document->RenderAtScale(2.0f);
            if (ZHLN::Test::ExpectTrue(doubled.has_value())) {
                ZHLN::Test::ExpectEq(doubled->width, 612u);
                ZHLN::Test::ExpectEq(doubled->height, 689u);
                ZHLN::Test::ExpectTrue(ExpectPixel(*doubled, 306, 60, 101, 154, 210));
                ZHLN::Test::ExpectTrue(ExpectPixel(*doubled, 306, 192, 255, 255, 255));
                ZHLN::Test::ExpectTrue(ExpectPixel(*doubled, 306, 600, 0, 68, 130));
            }

            // Half scale is 153 x 173 (ceil of 172.175). The C's band is 25
            // pixels wide there, so its middle is still a safe sample.
            auto halved = document->RenderAtScale(0.5f);
            if (ZHLN::Test::ExpectTrue(halved.has_value())) {
                ZHLN::Test::ExpectEq(halved->width, 153u);
                ZHLN::Test::ExpectEq(halved->height, 173u);
                ZHLN::Test::ExpectTrue(ExpectPixel(*halved, 76, 48, 255, 255, 255));
            }
            return {};
        }

        // --- 6. dpi moves physical units and leaves px alone ---
        std::expected<void, ZHLN::Error> dpi_leaves_pixel_units_alone() {
            ZHLN::SVG::Options    hires {.dpi = 192.0f};
            ZHLN::SVG::Rasterizer rasterizer(hires);
            if (!ZHLN::Test::ExpectTrue(rasterizer.IsValid())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(rasterizer.GetOptions().dpi, 192.0f);

            // The artwork is sized in px, which is absolute: doubling the dpi must
            // not double the document.
            auto logoAt192 = rasterizer.LoadString(kLogo);
            if (ZHLN::Test::ExpectTrue(logoAt192.has_value())) {
                ZHLN::Test::ExpectEq(logoAt192->NativeSize().width, 306.0f);
                ZHLN::Test::ExpectInRange(logoAt192->NativeSize().height, 344.34f, 344.36f);
            }

            // 1in x 0.5in is 96x48 at the default dpi and 192x96 at 192, which is
            // the same option doing the opposite job on a document that asks for
            // it.
            auto inchesAt96 = ZHLN::SVG::LoadString(kInches);
            if (ZHLN::Test::ExpectTrue(inchesAt96.has_value())) {
                ZHLN::Test::ExpectEq(inchesAt96->NativeSize().width, 96.0f);
                ZHLN::Test::ExpectEq(inchesAt96->NativeSize().height, 48.0f);
            }
            auto inchesAt192 = rasterizer.LoadString(kInches);
            if (ZHLN::Test::ExpectTrue(inchesAt192.has_value())) {
                ZHLN::Test::ExpectEq(inchesAt192->NativeSize().width, 192.0f);
                ZHLN::Test::ExpectEq(inchesAt192->NativeSize().height, 96.0f);
            }
            return {};
        }

        // --- 7. Exact geometry: the fixture with integer coordinates ---
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
            ZHLN::Test::ExpectTrue(raster->alpha == ZHLN::SVG::AlphaMode::Straight);

            ZHLN::Test::ExpectTrue(IsOpaqueBlue(raster->At(4, 8)));
            ZHLN::Test::ExpectTrue(IsOpaqueRed(raster->At(16, 8)));
            // Nothing is drawn on the right quarter, and resvg composites onto
            // whatever memory it is given: this is transparent only because the
            // wrapper zeroed the pixmap first.
            ZHLN::Test::ExpectTrue(IsTransparent(raster->At(28, 8)));
            return {};
        }

        // --- 8. Fit modes place the drawing where the arithmetic says ---
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

        // --- 9. Premultiplied out of resvg, straight out of the wrapper ---
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

        // --- 10. Rendering one node by id ---
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

            // The artwork has an id on its root element alone -- "Layer_1" -- and
            // its facets are unnamed, so there is nothing in it to select. What
            // the suite asserts is the refusal: a name that is certainly absent
            // comes back as NodeNotFound rather than rendering the whole document
            // or handing resvg a NULL id.
            auto logo = ZHLN::SVG::LoadString(kLogo);
            if (ZHLN::Test::ExpectTrue(logo.has_value())) {
                ZHLN::Test::ExpectFalse(logo->NodeExists("no_such_layer"));
                ZHLN::Test::ExpectFalse(logo->NodeBoundingBox("no_such_layer").has_value());
                const auto absent = logo->RenderNode("no_such_layer", 64, 64);
                ZHLN::Test::ExpectFalse(absent.has_value());
                if (!absent) {
                    ZHLN::Test::ExpectTrue(absent.error().Is(ZHLN::SVG::SVGError::NodeNotFound));
                }
            }
            return {};
        }

        // --- 11. Scale, and the inputs that must be refused ---
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

        // --- 12. resvg's failures arrive as SVGError, not as an abort ---
        std::expected<void, ZHLN::Error> bad_input_is_an_error_not_a_crash() {
            ZHLN::Test::ExpectFalse(ZHLN::SVG::LoadString(kNotSvg).has_value());
            ZHLN::Test::ExpectFalse(ZHLN::SVG::LoadString("").has_value());

            // Half of the artwork: usvg parses a document to the end before it
            // renders any of it, so a truncated one is an error rather than a
            // partial tree that happens to draw. Which SVGError it is depends on
            // where the cut lands in resvg's parser, so the assertion is on the
            // category, and the code is printed for whoever is reading the log.
            const auto truncated = ZHLN::SVG::LoadString(kLogo.substr(0, kLogo.size() / 2));
            ZHLN::Test::ExpectFalse(truncated.has_value());
            if (!truncated) {
                ZHLN::Test::ExpectTrue(truncated.error().Is<ZHLN::SVG::SVGError>());
                ZHLN::Println("    [SVG] {} bytes of {} -> {}", kLogo.size() / 2, kLogo.size(), truncated.error());
            }

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

        // --- 13. Options reach resvg, and a Rasterizer is reusable ---
        std::expected<void, ZHLN::Error> options_and_rasterizer_reuse() {
            // One Rasterizer, many documents: that reuse is the reason the type
            // exists, since the font database inside it is expensive to build.
            ZHLN::SVG::Rasterizer rasterizer;
            if (!ZHLN::Test::ExpectTrue(rasterizer.IsValid())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(rasterizer.GetOptions().dpi, 96.0f);

            auto                           artwork = rasterizer.LoadString(kLogo);
            auto                           bars    = rasterizer.LoadString(kBars);
            const std::span<const uint8_t> translucentBytes(reinterpret_cast<const uint8_t*>(kTranslucent.data()), kTranslucent.size());
            auto                           translucent = rasterizer.LoadData(translucentBytes);

            ZHLN::Test::ExpectTrue(artwork.has_value());
            ZHLN::Test::ExpectTrue(bars.has_value());
            ZHLN::Test::ExpectTrue(translucent.has_value());
            if (artwork) {
                ZHLN::Test::ExpectEq(artwork->NativeSize().width, 306.0f);
                // Parsing the next document must not have disturbed this one.
                auto still = artwork->Render(64, 64);
                ZHLN::Test::ExpectTrue(still.has_value());
                if (still) {
                    ZHLN::Test::ExpectEq(still->width, 64u);
                }
            }
            if (bars) {
                ZHLN::Test::ExpectEq(bars->NativeSize().width, 32.0f);
            }
            if (translucent) {
                ZHLN::Test::ExpectEq(translucent->NativeSize().width, 16.0f);
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
                const auto refused = withBrokenFonts.LoadString(kLogo);
                ZHLN::Test::ExpectFalse(refused.has_value());
                if (!refused) {
                    ZHLN::Test::ExpectTrue(refused.error().Is(ZHLN::SVG::SVGError::FontLoadFailed));
                }
            } else {
                ZHLN::Test::ExpectTrue(withBrokenFonts.LoadString(kLogo).has_value());
            }
            return {};
        }

        // --- 14. Ownership: a Document outlives the Rasterizer that parsed it ---
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
                auto                  loaded = rasterizer.LoadString(kLogo);
                if (!ZHLN::Test::ExpectTrue(loaded.has_value())) {
                    return std::unexpected(SVGTestError::ParseFailed);
                }
                document = std::move(*loaded);
            }

            ZHLN::Test::ExpectTrue(document.IsValid());
            auto afterScope = document.RenderFitted(100, 100);
            if (!ZHLN::Test::ExpectTrue(afterScope.has_value())) {
                return std::unexpected(SVGTestError::RenderFailed);
            }
            ZHLN::Test::ExpectTrue(afterScope->At(50, 50).a > 240);

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

        // --- 15. Files on disk, and the derived resource directory ---
        std::expected<void, ZHLN::Error> loads_a_file_from_disk() {
            const ScratchDir sandbox("file");
            const auto       path = sandbox.Write("art/cpp_logo.svg", kLogo);

            auto document = ZHLN::SVG::LoadFile(path.string());
            if (!ZHLN::Test::ExpectTrue(document.has_value())) {
                return std::unexpected(SVGTestError::ParseFailed);
            }
            ZHLN::Test::ExpectEq(document->NativeSize().width, 306.0f);

            // The one-shot loader, which is the call an asset pipeline makes:
            // Contain by default, straight alpha by default.
            auto raster = ZHLN::SVG::RasterizeFile(path.string(), 100, 100);
            if (ZHLN::Test::ExpectTrue(raster.has_value())) {
                ZHLN::Test::ExpectEq(raster->width, 100u);
                ZHLN::Test::ExpectTrue(IsTransparent(raster->At(1, 50)));
                ZHLN::Test::ExpectTrue(ExpectPixel(*raster, 50, 28, 255, 255, 255));
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
            ZHLN::Test::ExpectTrue(rasterizer.LoadString(kLogo).has_value());
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

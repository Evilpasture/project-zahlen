// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/RadianceMap.hpp>
#include <cstring>
#include <string>
#include <vector>

enum class RadianceTestError : uint8_t {
    DecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"A synthetic radiance blob did not decode to the expected pixels.">{}) = 1,
    CookedRoundTripFailed ZHLN_ANNOTATION(ZHLN::Description<"The cooked ZRD1 container did not round-trip.">{}),
    CacheFailed ZHLN_ANNOTATION(ZHLN::Description<"AssetManager did not cache or drop the radiance map.">{}),
};

namespace {

void Append(std::vector<std::byte>& out, std::string_view text) {
    for (const char c: text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

void AppendByte(std::vector<std::byte>& out, uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

// e = 136 -> scale 1. RGB bytes are the float values.
void AppendPixel(std::vector<std::byte>& out, uint8_t r, uint8_t g, uint8_t b) {
    AppendByte(out, r);
    AppendByte(out, g);
    AppendByte(out, b);
    AppendByte(out, 136);
}

std::vector<std::byte> FlatHdr(uint32_t width, uint32_t height, std::string_view ySign, float exposure, uint8_t r0, uint8_t r1) {
    std::vector<std::byte> out;
    Append(out, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n");
    Append(out, "EXPOSURE=");
    Append(out, exposure == 2.0f ? "2" : "1");
    Append(out, "\n\n");
    Append(out, ySign);
    Append(out, " ");
    Append(out, std::to_string(height));
    Append(out, " +X ");
    Append(out, std::to_string(width));
    Append(out, "\n");
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t r = y == 0 ? r0 : r1;
        for (uint32_t x = 0; x < width; ++x) {
            AppendPixel(out, r, 64, 32);
        }
    }
    return out;
}

std::vector<std::byte> RleHdr() {
    std::vector<std::byte> out;
    Append(out, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n");
    AppendByte(out, 2);
    AppendByte(out, 2);
    AppendByte(out, 0);
    AppendByte(out, 8);
    const uint8_t channels[4] = {128, 64, 32, 136};
    for (const uint8_t value: channels) {
        AppendByte(out, 128 + 8); // run of 8
        AppendByte(out, value);
    }
    return out;
}

} // namespace

struct RadianceTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> flat_rgbe_decodes_top_down() {
            const auto bytes = FlatHdr(4, 2, "-Y", 1.0f, 10, 20);
            auto decoded = ZHLN::DecodeRadiance(bytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->width, 4u) || !ZHLN::Test::ExpectEq(decoded->height, 2u)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            // e=136, exposure 1: the stored byte is the float.
            if (!ZHLN::Test::ExpectEq(decoded->rgba[0], 10.0f) || !ZHLN::Test::ExpectEq(decoded->rgba[1], 64.0f) ||
                !ZHLN::Test::ExpectEq(decoded->rgba[3], 1.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            // Second row starts at texel 4.
            if (!ZHLN::Test::ExpectEq(decoded->rgba[4u * 4u], 20.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            if (!ZHLN::Test::ExpectTrue(decoded->contentHash != 0)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> positive_y_is_flipped_to_top_down() {
            const auto bytes = FlatHdr(4, 2, "+Y", 1.0f, 10, 20);
            auto decoded = ZHLN::DecodeRadiance(bytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            // +Y writes the bottom row first, so the decoded top row is the file's second row.
            if (!ZHLN::Test::ExpectEq(decoded->rgba[0], 20.0f) || !ZHLN::Test::ExpectEq(decoded->rgba[4u * 4u], 10.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> exposure_scales_pixels() {
            const auto bytes = FlatHdr(4, 1, "-Y", 2.0f, 10, 10);
            auto decoded = ZHLN::DecodeRadiance(bytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->rgba[0], 20.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> negative_x_is_flipped() {
            // Width < 8, so the scanline is flat. The file stores the rightmost
            // column first; the decoder mirrors it back to +X on the left.
            std::vector<std::byte> bytes;
            Append(bytes, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 -X 4\n");
            const uint8_t reds[4] = {30, 20, 10, 1};
            for (const uint8_t red: reds) {
                AppendPixel(bytes, red, 0, 0);
            }
            auto decoded = ZHLN::DecodeRadiance(bytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->rgba[0], 1.0f) || !ZHLN::Test::ExpectEq(decoded->rgba[3u * 4u], 30.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> wide_scanline_without_rle_marker_is_flat() {
            std::vector<std::byte> bytes;
            Append(bytes, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n");
            for (uint32_t x = 0; x < 8; ++x) {
                AppendPixel(bytes, static_cast<uint8_t>(x + 1), 0, 0);
            }
            auto decoded = ZHLN::DecodeRadiance(bytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->width, 8u) || !ZHLN::Test::ExpectEq(decoded->rgba[0], 1.0f) ||
                !ZHLN::Test::ExpectEq(decoded->rgba[7u * 4u], 8.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rle_scanline_decodes() {
            auto decoded = ZHLN::DecodeRadiance(RleHdr());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->width, 8u) || !ZHLN::Test::ExpectEq(decoded->rgba[0], 128.0f) ||
                !ZHLN::Test::ExpectEq(decoded->rgba[7u * 4u + 1u], 64.0f)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cooked_container_round_trips() {
            auto decoded = ZHLN::DecodeRadiance(RleHdr());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            const auto cooked = ZHLN::EncodeCookedRadiance(*decoded);
            if (cooked.size() < 4 || static_cast<uint8_t>(cooked[0]) != 0x5A || static_cast<uint8_t>(cooked[1]) != 0x52 ||
                static_cast<uint8_t>(cooked[2]) != 0x44 || static_cast<uint8_t>(cooked[3]) != 0x31) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            auto again = ZHLN::DecodeRadiance(cooked);
            if (!again) {
                return std::unexpected(again.error());
            }
            if (!ZHLN::Test::ExpectEq(again->width, decoded->width) || !ZHLN::Test::ExpectEq(again->contentHash, decoded->contentHash) ||
                !ZHLN::Test::ExpectEq(again->rgba[0], decoded->rgba[0])) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> garbage_is_rejected() {
            const std::byte junk[] = {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
            auto decoded = ZHLN::DecodeRadiance(junk);
            if (!ZHLN::Test::ExpectTrue(!decoded.has_value())) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cache_drops_on_clear() {
            ZHLN::AssetManager assets;
            auto decoded = ZHLN::DecodeRadiance(RleHdr());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            const uint64_t id = ZHLN::HashAssetPath("environments/unit.hdr");
            assets.CacheRadiance(id, std::make_unique<ZHLN::RadianceMap>(std::move(*decoded)));
            if (assets.GetCachedRadiance(id) == nullptr) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            assets.ClearCache();
            if (assets.GetCachedRadiance(id) != nullptr) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            return {};
        }
    };
};

auto RunRadianceSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RadianceTestSuite>();
}

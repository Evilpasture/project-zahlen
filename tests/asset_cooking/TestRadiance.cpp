// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/CookerFixture.hpp"
#include <AssetCooking/EnvironmentPreparation.hpp>
#include <AssetCooking/RadianceDecoder.hpp>
#include <AssetCooking/RadianceEncoder.hpp>
#include <Zahlen/AssetManager.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class RadianceTestError : uint8_t {
    DecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"A synthetic radiance blob did not decode to the expected pixels.">{}) = 1,
    CookedRoundTripFailed ZHLN_ANNOTATION(ZHLN::Description<"The cooked ZRD2 container did not round-trip.">{}),
    CacheFailed ZHLN_ANNOTATION(ZHLN::Description<"AssetManager did not cache or drop the radiance map.">{}),
    FixtureFailed ZHLN_ANNOTATION(ZHLN::Description<"Could not write or cook the synthetic radiance fixture.">{}),
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

[[nodiscard]] auto HotspotEnvironment(bool secondSun = false) -> ZHLN::EnvironmentImage {
    ZHLN::EnvironmentImage image;
    image.width = 128;
    image.height = 64;
    image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4u);
    for (size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i] = 0.20f;
        image.rgba[i + 1] = 0.25f;
        image.rgba[i + 2] = 0.30f;
        image.rgba[i + 3] = 1.0f;
    }
    const auto set = [&](uint32_t x, uint32_t y) {
        const size_t i = (static_cast<size_t>(y) * image.width + x) * 4u;
        image.rgba[i] = 50000.0f;
        image.rgba[i + 1] = 38000.0f;
        image.rgba[i + 2] = 12000.0f;
    };
    set(52, 32);
    if (secondSun) set(20, 32);
    return image;
}

// 2x1 JPEG, both texels sRGB (128,64,32), quality 100, generated with:
// convert -size 2x1 xc:'#804020' -sampling-factor 4:4:4 -quality 100 ...jpg
// Lossy decoding yields (128,63,31) with stb_image. Embed the tiny fixture so
// the CPU radiance test does not depend on the Khronos checkout or cwd.
constexpr std::array<uint8_t, 287> kLdrJpeg {{
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0xff, 0xc0,
    0x00, 0x11, 0x08, 0x00, 0x01, 0x00, 0x02, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
    0x01, 0xff, 0xc4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xc4, 0x00,
    0x15, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x08, 0x09, 0xff, 0xc4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01,
    0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00, 0x1d, 0xc2, 0x75, 0x18, 0x7f, 0xff, 0xd9,
}};

bool WriteRadianceFile(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

struct RadianceFixture {
    TempSandbox sandbox {"radiance"};
    ZHLN::AssetManager assets;

    [[nodiscard]] auto Decode(std::span<const std::byte> bytes, std::string_view name = "environment.hdr")
        -> std::expected<ZHLN::EnvironmentImageView, ZHLN::ErrorCode> {
        const fs::path path = sandbox.SubPath(name);
        if (!WriteRadianceFile(path, bytes)) {
            return std::unexpected(RadianceTestError::FixtureFailed);
        }
        auto decoded = ZHLN::AssetCooking::ReadEnvironmentImage(assets.VFS(), path.string());
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (!assets.CacheEnvironmentImage(path.string(), std::move(*decoded))) {
            return std::unexpected(RadianceTestError::CacheFailed);
        }
        return *assets.FindEnvironmentImage(path.string());
    }
};

} // namespace

struct RadianceTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> flat_rgbe_decodes_top_down() {
            const auto bytes = FlatHdr(4, 2, "-Y", 1.0f, 10, 20);
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes);
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
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes);
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
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes);
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
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes);
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
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes);
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
            RadianceFixture fixture;
            auto decoded = fixture.Decode(RleHdr());
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
            RadianceFixture fixture;
            const fs::path rawPath    = fixture.sandbox.SubPath("environment.hdr");
            const fs::path cookedPath = fixture.sandbox.SubPath("environment.zrd");
            if (!WriteRadianceFile(rawPath, RleHdr())) {
                return std::unexpected(RadianceTestError::FixtureFailed);
            }
            const fs::path zcook = FindZcookExecutable();
            if (zcook.empty() || RunZcook(zcook, std::format(R"(tex -i "{}" -o "{}")", rawPath.string(), cookedPath.string())) != 0) {
                return std::unexpected(RadianceTestError::FixtureFailed);
            }
            std::ifstream cookedFile(cookedPath, std::ios::binary);
            std::array<uint32_t, 7> header {};
            cookedFile.read(reinterpret_cast<char*>(header.data()), sizeof(header));
            if (!ZHLN::Test::ExpectTrue(cookedFile.good()) || !ZHLN::Test::ExpectEq(header[0], 0x3244525Au) ||
                !ZHLN::Test::ExpectEq(header[1], 2u) || !ZHLN::Test::ExpectEq(header[2], 8u) ||
                !ZHLN::Test::ExpectEq(header[3], 1u) || !ZHLN::Test::ExpectEq(header[4], 8u * 4u * sizeof(float)) ||
                !ZHLN::Test::ExpectEq(header[5], 0u) || !ZHLN::Test::ExpectEq(header[6], 0u)) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            auto decoded = ZHLN::AssetCooking::ReadEnvironmentImage(fixture.assets.VFS(), rawPath.string());
            auto again   = ZHLN::AssetCooking::ReadEnvironmentImage(fixture.assets.VFS(), cookedPath.string());
            if (!decoded || !again) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            if (!ZHLN::Test::ExpectEq(again->width, decoded->width) || !ZHLN::Test::ExpectEq(again->height, decoded->height) ||
                !ZHLN::Test::ExpectEq(again->contentHash, decoded->contentHash) ||
                !ZHLN::Test::ExpectTrue(std::ranges::equal(again->rgba, decoded->rgba))) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cooked_sun_separates_diffuse_and_specular_without_changing_sky() {
            auto image = HotspotEnvironment();
            const auto visibleSky = image.rgba;
            ZHLN::AssetCooking::PrepareEnvironmentImage(image);
            const size_t hotspot = (32u * 128u + 52u) * 4u;
            if (!image.sun || image.lightingRgba.size() != image.rgba.size() ||
                !ZHLN::Test::ExpectEq(image.rgba[hotspot], 50000.0f) ||
                !ZHLN::Test::ExpectLt(image.lightingRgba[hotspot], 1.0f) ||
                !ZHLN::Test::ExpectGt(image.sun->irradiance[0], 30.0f) ||
                !ZHLN::Test::ExpectLt(image.sun->direction[2], -0.4f) ||
                !std::ranges::equal(image.rgba, visibleSky)) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            // N=+Z faces away from the extracted sun. Its diffuse SH must be
            // the original flat RGB sky, not the teal/clamped SH of the spike.
            for (int channel = 0; channel < 3; ++channel) {
                const auto& sh = image.sun->diffuseSH;
                const float irradiance = sh[0][channel] * 0.282095f + sh[2][channel] * 0.488603f + sh[6][channel] * 0.630784f;
                if (std::abs(irradiance - visibleSky[channel]) > 0.005f)
                    return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            // Idempotent for host callers; a second pass must never extract
            // another sun from the conditioned copy or change the visible HDR.
            ZHLN::AssetCooking::PrepareEnvironmentImage(image);
            const auto cooked = ZHLN::AssetCooking::EncodeCookedRadiance(image);
            auto restored = ZHLN::AssetCooking::DecodeRadiance(cooked);
            if (cooked.empty() || !restored || !restored->sun || restored->contentHash == 0 ||
                !std::ranges::equal(restored->rgba, image.rgba) ||
                !std::ranges::equal(restored->lightingRgba, image.lightingRgba) ||
                !ZHLN::Test::ExpectEq(restored->sun->direction[0], image.sun->direction[0]) ||
                !ZHLN::Test::ExpectEq(restored->sun->irradiance[1], image.sun->irradiance[1]) ||
                !ZHLN::Test::ExpectEq(restored->sun->diffuseSH[3][2], image.sun->diffuseSH[3][2])) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            // Cooked metadata/lengths must not be treated as trusted input.
            auto bad = cooked;
            const float invalidDir = std::numeric_limits<float>::quiet_NaN();
            std::memcpy(bad.data() + 7u * sizeof(uint32_t), &invalidDir, sizeof(invalidDir));
            if (ZHLN::AssetCooking::DecodeRadiance(bad)) return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            bad = cooked;
            const uint32_t missingLighting = 0;
            std::memcpy(bad.data() + 5u * sizeof(uint32_t), &missingLighting, sizeof(missingLighting));
            if (ZHLN::AssetCooking::DecodeRadiance(bad)) return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            bad = cooked;
            bad.pop_back();
            if (ZHLN::AssetCooking::DecodeRadiance(bad)) return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> legacy_zrd1_conditions_only_at_host_load() {
            const auto image = HotspotEnvironment();
            const std::array<uint32_t, 5> header {0x3144525Au, 1u, image.width, image.height,
                                                  static_cast<uint32_t>(image.rgba.size() * sizeof(float))};
            std::vector<std::byte> legacy(sizeof(header) + image.rgba.size() * sizeof(float));
            std::memcpy(legacy.data(), header.data(), sizeof(header));
            std::memcpy(legacy.data() + sizeof(header), image.rgba.data(), image.rgba.size() * sizeof(float));
            auto prepared = ZHLN::AssetCooking::DecodeRadiance(legacy);
            if (!prepared || !prepared->sun || prepared->lightingRgba.size() != image.rgba.size() ||
                !ZHLN::Test::ExpectEq(prepared->rgba[(32u * 128u + 52u) * 4u], 50000.0f)) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            const auto newFormat = ZHLN::AssetCooking::EncodeCookedRadiance(*prepared);
            auto reloaded = ZHLN::AssetCooking::DecodeRadiance(newFormat);
            if (!reloaded || !reloaded->sun || !ZHLN::Test::ExpectEq(reloaded->contentHash, prepared->contentHash) ||
                !std::ranges::equal(reloaded->lightingRgba, prepared->lightingRgba)) {
                return std::unexpected(RadianceTestError::CookedRoundTripFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> multiple_bright_sources_stay_in_ordinary_ibl() {
            auto image = HotspotEnvironment(true);
            ZHLN::AssetCooking::PrepareEnvironmentImage(image);
            if (image.sun || !image.lightingRgba.empty()) return std::unexpected(RadianceTestError::DecodeFailed);
            const auto cooked = ZHLN::AssetCooking::EncodeCookedRadiance(image);
            auto decoded = ZHLN::AssetCooking::DecodeRadiance(cooked);
            if (!decoded || decoded->sun || !decoded->lightingRgba.empty() ||
                !std::ranges::equal(decoded->rgba, image.rgba)) return std::unexpected(RadianceTestError::DecodeFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> ldr_jpeg_is_linearized_for_ibl() {
            const auto bytes = std::as_bytes(std::span {kLdrJpeg});
            RadianceFixture fixture;
            auto decoded = fixture.Decode(bytes, "environment.jpg");
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            if (!ZHLN::Test::ExpectEq(decoded->width, 2u) || !ZHLN::Test::ExpectEq(decoded->height, 1u) ||
                !ZHLN::Test::ExpectEq(decoded->rgba.size(), 8u) || decoded->sun || !decoded->lightingRgba.empty()) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            // sRGB 128 must become ~0.216, not 128/255 or stb_image's
            // approximate gamma-2.2 conversion. JPEG's other channels may
            // differ by a byte after YCbCr quantization.
            if (!ZHLN::Test::ExpectLt(std::abs(decoded->rgba[0] - 0.2158605f), 0.002f) ||
                !ZHLN::Test::ExpectLt(std::abs(decoded->rgba[1] - 0.0497066f), 0.003f) ||
                !ZHLN::Test::ExpectLt(std::abs(decoded->rgba[2] - 0.0137021f), 0.002f) ||
                !ZHLN::Test::ExpectEq(decoded->rgba[3], 1.0f) || !ZHLN::Test::ExpectEq(decoded->rgba[4], decoded->rgba[0]) ||
                !ZHLN::Test::ExpectTrue(decoded->contentHash != 0)) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> truncated_ldr_jpeg_is_rejected() {
            const auto headerOnly = std::as_bytes(std::span {kLdrJpeg}).first(3);
            RadianceFixture fixture;
            auto decoded = fixture.Decode(headerOnly, "truncated.jpg");
            if (!ZHLN::Test::ExpectFalse(decoded.has_value()) ||
                !ZHLN::Test::ExpectEq(ZHLN::Error(decoded.error()).Name(), std::string_view {"LdrDecodeFailed"})) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> garbage_is_rejected() {
            const std::byte junk[] = {std::byte {1}, std::byte {2}, std::byte {3}, std::byte {4}};
            RadianceFixture fixture;
            auto decoded = fixture.Decode(junk);
            if (!ZHLN::Test::ExpectTrue(!decoded.has_value())) {
                return std::unexpected(RadianceTestError::DecodeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> cache_drops_on_clear() {
            RadianceFixture fixture;
            const fs::path path = fixture.sandbox.SubPath("environments/unit.hdr");
            constexpr std::string_view virtualPath = "environments/unit.hdr";
            if (!fixture.assets.MountDirectory(fixture.sandbox.rootPath.string()) || !WriteRadianceFile(path, RleHdr())) {
                return std::unexpected(RadianceTestError::FixtureFailed);
            }
            auto decoded = ZHLN::AssetCooking::ReadEnvironmentImage(fixture.assets.VFS(), virtualPath);
            if (!decoded || !fixture.assets.CacheEnvironmentImage(virtualPath, std::move(*decoded))) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            const auto first = fixture.assets.FindEnvironmentImage(virtualPath);
            if (!first || !ZHLN::Test::ExpectEq(first->width, 8u)) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            if (!WriteRadianceFile(path, FlatHdr(4, 1, "-Y", 1.0f, 10, 10))) {
                return std::unexpected(RadianceTestError::FixtureFailed);
            }
            const auto cached = fixture.assets.FindEnvironmentImage(virtualPath);
            if (!cached || !ZHLN::Test::ExpectEq(cached->rgba.data(), first->rgba.data()) ||
                !ZHLN::Test::ExpectEq(cached->width, 8u)) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            const uint64_t oldHash = first->contentHash;
            fixture.assets.ClearCache(); // invalidates both borrowed views
            if (!ZHLN::Test::ExpectFalse(fixture.assets.FindEnvironmentImage(virtualPath).has_value())) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            auto newPixels = ZHLN::AssetCooking::ReadEnvironmentImage(fixture.assets.VFS(), virtualPath);
            if (!newPixels || !fixture.assets.CacheEnvironmentImage(virtualPath, std::move(*newPixels))) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            const auto reloaded = fixture.assets.FindEnvironmentImage(virtualPath);
            if (!reloaded || !ZHLN::Test::ExpectEq(reloaded->width, 4u) ||
                !ZHLN::Test::ExpectEq(reloaded->rgba[0], 10.0f) ||
                !ZHLN::Test::ExpectNe(reloaded->contentHash, oldHash)) {
                return std::unexpected(RadianceTestError::CacheFailed);
            }
            return {};
        }
    };
};

auto RunRadianceSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RadianceTestSuite>();
}

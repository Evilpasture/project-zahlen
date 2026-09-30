// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RadianceDecoder.hpp"
#include "CookedRadianceFormat.hpp"
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <stb_image.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ZHLN::AssetCooking {

namespace {

constexpr uint32_t kMaxRadianceExtent = 8192;
constexpr size_t kMaxRadianceFileBytes = 512u * 1024u * 1024u;

using RadianceMap = EnvironmentImage;

[[nodiscard]] auto HashRadiancePixels(const float* rgba, uint32_t width, uint32_t height) noexcept -> uint64_t {
    uint64_t hash = Hash64(reinterpret_cast<const char*>(&width), sizeof(width));
    hash ^= Hash64(reinterpret_cast<const char*>(&height), sizeof(height)) + kGolden64 + (hash << 6) + (hash >> 2);
    const size_t bytes = sizeof(float) * 4u * static_cast<size_t>(width) * static_cast<size_t>(height);
    hash ^= Hash64(reinterpret_cast<const char*>(rgba), bytes) + kGolden64 + (hash << 6) + (hash >> 2);
    return hash == 0 ? 1 : hash;
}

struct Cursor {
    std::span<const std::byte> bytes;
    size_t                     pos = 0;

    [[nodiscard]] auto Remain() const noexcept -> size_t {
        return pos < bytes.size() ? bytes.size() - pos : 0;
    }

    auto Read(uint8_t& out) noexcept -> bool {
        if (pos >= bytes.size()) {
            return false;
        }
        out = static_cast<uint8_t>(bytes[pos++]);
        return true;
    }

    auto ReadLine(std::string& out) -> bool {
        out.clear();
        if (pos >= bytes.size()) {
            return false;
        }
        while (pos < bytes.size()) {
            const char c = static_cast<char>(bytes[pos++]);
            if (c == '\n') {
                break;
            }
            if (c != '\r') {
                out.push_back(c);
            }
        }
        return true;
    }
};

void RgbeToFloat(const uint8_t rgbe[4], float* dst, float exposure) noexcept {
    if (rgbe[3] == 0) {
        dst[0] = 0.0f;
        dst[1] = 0.0f;
        dst[2] = 0.0f;
    } else {
        const float scale = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8)) * exposure;
        dst[0]            = static_cast<float>(rgbe[0]) * scale;
        dst[1]            = static_cast<float>(rgbe[1]) * scale;
        dst[2]            = static_cast<float>(rgbe[2]) * scale;
    }
    dst[3] = 1.0f;
}

auto ParseU32(std::string_view text, uint32_t& out) noexcept -> bool {
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc {} && ptr != text.data();
}

auto ParseResolution(std::string_view line, uint32_t& width, uint32_t& height, bool& flipX, bool& flipY) -> bool {
    width = height = 0;
    flipX = flipY = false;
    size_t i = 0;
    const auto skip = [&] {
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
    };
    bool sawX = false;
    bool sawY = false;
    for (int axis = 0; axis < 2; ++axis) {
        skip();
        if (i >= line.size() || (line[i] != '+' && line[i] != '-')) {
            return false;
        }
        const bool positive = line[i] == '+';
        ++i;
        if (i >= line.size() || (line[i] != 'X' && line[i] != 'Y')) {
            return false;
        }
        const bool isX = line[i] == 'X';
        ++i;
        skip();
        const size_t start = i;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
            ++i;
        }
        uint32_t value = 0;
        if (start == i || !ParseU32(line.substr(start, i - start), value)) {
            return false;
        }
        if (isX) {
            if (sawX) {
                return false;
            }
            sawX  = true;
            width = value;
            flipX = !positive;
        } else {
            if (sawY) {
                return false;
            }
            sawY   = true;
            height = value;
            flipY  = positive;
        }
    }
    return sawX && sawY && width > 0 && height > 0;
}

auto ReadFlatPixels(Cursor& cur, float* dst, uint32_t count, float exposure) -> bool {
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t rgbe[4];
        for (uint8_t& b: rgbe) {
            if (!cur.Read(b)) {
                return false;
            }
        }
        RgbeToFloat(rgbe, dst + static_cast<size_t>(i) * 4u, exposure);
    }
    return true;
}

auto ReadRleChannel(Cursor& cur, uint8_t* dst, uint32_t width, int channel) -> bool {
    uint32_t written = 0;
    while (written < width) {
        uint8_t count = 0;
        if (!cur.Read(count) || count == 0) {
            return false;
        }
        if (count > 128) {
            const uint32_t run = static_cast<uint32_t>(count) - 128u;
            uint8_t        value = 0;
            if (!cur.Read(value) || written + run > width) {
                return false;
            }
            for (uint32_t i = 0; i < run; ++i) {
                dst[(static_cast<size_t>(written + i) * 4u) + static_cast<size_t>(channel)] = value;
            }
            written += run;
        } else {
            const uint32_t dump = count;
            if (written + dump > width) {
                return false;
            }
            for (uint32_t i = 0; i < dump; ++i) {
                uint8_t value = 0;
                if (!cur.Read(value)) {
                    return false;
                }
                dst[(static_cast<size_t>(written + i) * 4u) + static_cast<size_t>(channel)] = value;
            }
            written += dump;
        }
    }
    return true;
}

auto DecodeRgbe(std::span<const std::byte> bytes) -> std::expected<RadianceMap, ErrorCode> {
    Cursor cur {.bytes = bytes};
    std::string line;
    if (!cur.ReadLine(line) || (line.rfind("#?RADIANCE", 0) != 0 && line.rfind("#?RGBE", 0) != 0)) {
        return std::unexpected(RadianceAssetError::BadHeader);
    }

    bool  formatOk = false;
    float exposure = 1.0f;
    for (;;) {
        if (!cur.ReadLine(line)) {
            return std::unexpected(RadianceAssetError::Truncated);
        }
        if (line.empty()) {
            break;
        }
        if (line == "FORMAT=32-bit_rle_rgbe") {
            formatOk = true;
        } else if (line.rfind("EXPOSURE=", 0) == 0) {
            float        value = 0.0f;
            const auto   tail  = std::string_view(line).substr(9);
            const auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), value);
            (void)ptr;
            if (ec == std::errc {} && value > 0.0f) {
                exposure *= value;
            }
        }
    }
    if (!formatOk) {
        return std::unexpected(RadianceAssetError::UnsupportedFormat);
    }
    if (!cur.ReadLine(line)) {
        return std::unexpected(RadianceAssetError::Truncated);
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool     flipX = false;
    bool     flipY = false;
    if (!ParseResolution(line, width, height, flipX, flipY) || width > kMaxRadianceExtent || height > kMaxRadianceExtent) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }

    RadianceMap map;
    map.width  = width;
    map.height = height;
    map.rgba.resize(static_cast<size_t>(width) * height * 4u);

    const bool flat = width < 8 || width >= 32768;
    if (flat) {
        if (!ReadFlatPixels(cur, map.rgba.data(), width * height, exposure)) {
            return std::unexpected(RadianceAssetError::Truncated);
        }
    } else {
        std::vector<uint8_t> scanline(static_cast<size_t>(width) * 4u);
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t c1 = 0;
            uint8_t c2 = 0;
            uint8_t c3 = 0;
            uint8_t c4 = 0;
            if (!cur.Read(c1) || !cur.Read(c2) || !cur.Read(c3) || !cur.Read(c4)) {
                return std::unexpected(RadianceAssetError::Truncated);
            }
            const bool newRle = c1 == 2 && c2 == 2 && (c3 & 0x80u) == 0;
            float*     row    = map.rgba.data() + static_cast<size_t>(y) * width * 4u;
            if (!newRle) {
                const uint8_t first[4] = {c1, c2, c3, c4};
                RgbeToFloat(first, row, exposure);
                if (width > 1 && !ReadFlatPixels(cur, row + 4, width - 1, exposure)) {
                    return std::unexpected(RadianceAssetError::Truncated);
                }
                continue;
            }
            const uint32_t declared = (static_cast<uint32_t>(c3) << 8u) | c4;
            if (declared != width) {
                return std::unexpected(RadianceAssetError::RleCorrupt);
            }
            for (int channel = 0; channel < 4; ++channel) {
                if (!ReadRleChannel(cur, scanline.data(), width, channel)) {
                    return std::unexpected(RadianceAssetError::RleCorrupt);
                }
            }
            for (uint32_t x = 0; x < width; ++x) {
                RgbeToFloat(scanline.data() + static_cast<size_t>(x) * 4u, row + static_cast<size_t>(x) * 4u, exposure);
            }
        }
    }

    if (flipY) {
        std::vector<float> row(static_cast<size_t>(width) * 4u);
        for (uint32_t y = 0; y < height / 2u; ++y) {
            float* a = map.rgba.data() + static_cast<size_t>(y) * width * 4u;
            float* b = map.rgba.data() + static_cast<size_t>(height - 1u - y) * width * 4u;
            std::memcpy(row.data(), a, row.size() * sizeof(float));
            std::memcpy(a, b, row.size() * sizeof(float));
            std::memcpy(b, row.data(), row.size() * sizeof(float));
        }
    }
    if (flipX) {
        for (uint32_t y = 0; y < height; ++y) {
            float* row = map.rgba.data() + static_cast<size_t>(y) * width * 4u;
            for (uint32_t x = 0; x < width / 2u; ++x) {
                float* a = row + static_cast<size_t>(x) * 4u;
                float* b = row + static_cast<size_t>(width - 1u - x) * 4u;
                for (int c = 0; c < 4; ++c) {
                    std::swap(a[c], b[c]);
                }
            }
        }
    }

    map.contentHash = HashRadiancePixels(map.rgba.data(), width, height);
    return map;
}

auto DecodeCooked(std::span<const std::byte> bytes) -> std::expected<RadianceMap, ErrorCode> {
    if (bytes.size() < sizeof(CookedRadianceHeader)) {
        return std::unexpected(RadianceAssetError::Truncated);
    }
    CookedRadianceHeader header {};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kCookedRadianceMagic) {
        return std::unexpected(RadianceAssetError::BadMagic);
    }
    if (header.version != kCookedRadianceVersion) {
        return std::unexpected(RadianceAssetError::UnsupportedVersion);
    }
    if (header.width == 0 || header.height == 0 || header.width > kMaxRadianceExtent || header.height > kMaxRadianceExtent) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }
    const size_t pixels = static_cast<size_t>(header.width) * header.height * 4u;
    const size_t bytesNeeded = pixels * sizeof(float);
    if (header.dataSize != bytesNeeded || bytes.size() < sizeof(header) + bytesNeeded) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }
    RadianceMap map;
    map.width  = header.width;
    map.height = header.height;
    map.rgba.resize(pixels);
    std::memcpy(map.rgba.data(), bytes.data() + sizeof(header), bytesNeeded);
    map.contentHash = HashRadiancePixels(map.rgba.data(), map.width, map.height);
    return map;
}

// Khronos' MetalRoughSpheres-LDR intentionally names a JPEG equirect instead
// of an HDR file. Convert its display-space sRGB texels to the linear float4
// data consumed by the same IBL bake as RGBE/ZRD1; do not substitute the HDR
// panorama, which would change what that fidelity scenario measures.
auto DecodeLdrJpeg(std::span<const std::byte> bytes) -> std::expected<RadianceMap, ErrorCode> {
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int   length = static_cast<int>(bytes.size());
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, length, &width, &height, &channels)) {
        return std::unexpected(RadianceAssetError::LdrDecodeFailed);
    }
    if (width <= 0 || height <= 0 || width > static_cast<int>(kMaxRadianceExtent) || height > static_cast<int>(kMaxRadianceExtent) ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > kMaxRadianceFileBytes / (4u * sizeof(float))) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }

    int decodedWidth = 0;
    int decodedHeight = 0;
    int decodedChannels = 0;
    auto pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> {
        stbi_load_from_memory(data, length, &decodedWidth, &decodedHeight, &decodedChannels, 3), &stbi_image_free
    };
    if (!pixels || decodedWidth != width || decodedHeight != height) {
        return std::unexpected(RadianceAssetError::LdrDecodeFailed);
    }

    // stb_image's float LDR path defaults to gamma 2.2, not the piecewise
    // sRGB transfer function. Decode bytes and linearize explicitly instead.
    static const auto srgbToLinear = [] {
        std::array<float, 256> table {};
        for (size_t i = 0; i < table.size(); ++i) {
            const float s = static_cast<float>(i) / 255.0f;
            table[i] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
        }
        return table;
    }();

    RadianceMap map;
    map.width = static_cast<uint32_t>(width);
    map.height = static_cast<uint32_t>(height);
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    map.rgba.resize(count * 4u);
    for (size_t i = 0; i < count; ++i) {
        for (size_t c = 0; c < 3; ++c) {
            map.rgba[i * 4u + c] = srgbToLinear[pixels.get()[i * 3u + c]];
        }
        map.rgba[i * 4u + 3u] = 1.0f;
    }
    map.contentHash = HashRadiancePixels(map.rgba.data(), map.width, map.height);
    return map;
}

auto ReadAssetBytes(const FS::VirtualFileSystem& vfs, std::string_view path) -> std::expected<std::vector<std::byte>, ErrorCode> {
    const size_t vfsSize = vfs.ReadFile(path, nullptr, 0);
    if (vfsSize > 0 && vfsSize <= kMaxRadianceFileBytes) {
        std::vector<std::byte> bytes(vfsSize);
        if (vfs.ReadFile(path, bytes.data(), bytes.size()) == vfsSize) {
            return bytes;
        }
    }

    std::ifstream in {std::string(path), std::ios::binary | std::ios::ate};
    if (!in) {
        return std::unexpected(RadianceAssetError::NotFound);
    }
    const auto end = in.tellg();
    if (end <= 0) {
        return std::unexpected(RadianceAssetError::Truncated);
    }
    const auto size = static_cast<size_t>(end);
    if (size > kMaxRadianceFileBytes) {
        return std::unexpected(RadianceAssetError::BadDimensions);
    }
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in) {
        return std::unexpected(RadianceAssetError::Truncated);
    }
    return bytes;
}

}

auto DecodeRadiance(std::span<const std::byte> bytes) -> std::expected<EnvironmentImage, ErrorCode> {
    if (bytes.size() >= sizeof(uint32_t)) {
        uint32_t magic = 0;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        if (magic == kCookedRadianceMagic) {
            return DecodeCooked(bytes);
        }
    }
    if (bytes.size() >= 3 && bytes[0] == std::byte {0xff} && bytes[1] == std::byte {0xd8} && bytes[2] == std::byte {0xff}) {
        return DecodeLdrJpeg(bytes);
    }
    return DecodeRgbe(bytes);
}

auto ReadEnvironmentImage(const FS::VirtualFileSystem& vfs, std::string_view path) -> std::expected<EnvironmentImage, ErrorCode> {
    if (path.empty()) {
        return std::unexpected(RadianceAssetError::NotFound);
    }
    auto bytes = ReadAssetBytes(vfs, path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return DecodeRadiance(*bytes);
}

} // namespace ZHLN::AssetCooking

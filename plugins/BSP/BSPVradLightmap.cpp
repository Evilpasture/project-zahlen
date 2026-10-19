// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BSPVradLightmap.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ZHLN::BSP {
namespace {

[[nodiscard]] auto FloatToHalf(float value) noexcept -> uint16_t {
    // VRAD's RGBExp payload is finite, but clamp defensively before converting
    // to binary16 so a malformed BSP cannot turn an atlas page into NaNs.
    value = std::clamp(value, 0.0f, 65504.0f);
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t        exp  = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t       mant = bits & 0x7FFFFFu;

    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mant = (mant | 0x800000u) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | ((mant + 0x1000u) >> 13u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7BFFu);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10u) | ((mant + 0x1000u) >> 13u));
}

struct PendingChart {
    size_t   faceIndex = 0;
    uint32_t width     = 0;
    uint32_t height    = 0;
    size_t   sourceLuxelOffset = 0;
    uint32_t page      = 0;
    uint32_t outerX    = 0;
    uint32_t outerY    = 0;
};

struct PendingPage {
    uint32_t cursorX   = 0;
    uint32_t cursorY   = 0;
    uint32_t rowHeight = 0;
    uint32_t usedWidth = 0;
    uint32_t usedHeight = 0;
};

[[nodiscard]] auto HasBaseStyle(const DFace& face) noexcept -> bool {
    // 255 is Source's LIGHTSTYLE_NONE. A valid offset with this style means
    // the face deliberately did not receive a static lightmap.
    return face.styles[0] != 255u;
}

} // namespace

auto DecodeVradLuxel(ColorRGBExp32 luxel) noexcept -> std::array<float, 3> {
    const float scale = std::ldexp(1.0f, static_cast<int>(luxel.exponent));
    return {
        static_cast<float>(luxel.r) * scale,
        static_cast<float>(luxel.g) * scale,
        static_cast<float>(luxel.b) * scale,
    };
}

auto BuildVradLightmapAtlas(const BSPMap& map, uint32_t maxPageSize, uint32_t gutter) -> VradLightmapAtlas {
    VradLightmapAtlas result;
    result.hdrSource = map.lightingIsHdr;
    result.faceCharts.resize(map.faces.size());

    if (map.lighting.empty() || maxPageSize == 0u) {
        return result;
    }

    // Pages are never rotated: UV1 follows Source's S/T basis, and rotating a
    // chart would need an additional per-part matrix for no packing benefit on
    // the mostly-square VRAD charts we receive.
    std::vector<PendingPage>  pages;
    std::vector<PendingChart> charts;
    pages.emplace_back();

    for (size_t faceIndex = 0; faceIndex < map.faces.size(); ++faceIndex) {
        const DFace& face = map.faces[faceIndex];
        if (face.lightofs < 0 || !HasBaseStyle(face) || face.lightmapTextureSizeInLuxels[0] < 0 || face.lightmapTextureSizeInLuxels[1] < 0) {
            continue;
        }

        const uint64_t width64  = static_cast<uint64_t>(face.lightmapTextureSizeInLuxels[0]) + 1u;
        const uint64_t height64 = static_cast<uint64_t>(face.lightmapTextureSizeInLuxels[1]) + 1u;
        if (width64 == 0u || height64 == 0u || width64 > std::numeric_limits<uint32_t>::max() || height64 > std::numeric_limits<uint32_t>::max()) {
            ++result.rejectedFaceCount;
            continue;
        }
        const uint32_t width  = static_cast<uint32_t>(width64);
        const uint32_t height = static_cast<uint32_t>(height64);
        const uint64_t sourceOffset = static_cast<uint64_t>(face.lightofs) / sizeof(ColorRGBExp32);
        const uint64_t sampleCount  = width64 * height64;
        if (static_cast<uint64_t>(face.lightofs) % sizeof(ColorRGBExp32) != 0u || sourceOffset + sampleCount > map.lighting.size()) {
            ++result.rejectedFaceCount;
            continue;
        }

        const uint64_t outerW64 = width64 + static_cast<uint64_t>(gutter) * 2u;
        const uint64_t outerH64 = height64 + static_cast<uint64_t>(gutter) * 2u;
        if (outerW64 > maxPageSize || outerH64 > maxPageSize || outerW64 > std::numeric_limits<uint32_t>::max() ||
            outerH64 > std::numeric_limits<uint32_t>::max()) {
            ++result.rejectedFaceCount;
            continue;
        }
        const uint32_t outerW = static_cast<uint32_t>(outerW64);
        const uint32_t outerH = static_cast<uint32_t>(outerH64);

        PendingPage* page = &pages.back();
        if (page->cursorX + outerW > maxPageSize) {
            page->cursorX = 0;
            page->cursorY += page->rowHeight;
            page->rowHeight = 0;
        }
        if (page->cursorY + outerH > maxPageSize) {
            pages.emplace_back();
            page = &pages.back();
        }

        PendingChart chart;
        chart.faceIndex         = faceIndex;
        chart.width             = width;
        chart.height            = height;
        chart.sourceLuxelOffset = static_cast<size_t>(sourceOffset);
        chart.page              = static_cast<uint32_t>(pages.size() - 1u);
        chart.outerX            = page->cursorX;
        chart.outerY            = page->cursorY;
        charts.push_back(chart);

        page->cursorX += outerW;
        page->rowHeight = std::max(page->rowHeight, outerH);
        page->usedWidth = std::max(page->usedWidth, page->cursorX);
        page->usedHeight = std::max(page->usedHeight, page->cursorY + outerH);
        result.luxelCount += static_cast<uint32_t>(sampleCount);
    }

    if (charts.empty()) {
        result.pages.clear();
        return result;
    }

    result.pages.resize(pages.size());
    for (size_t i = 0; i < pages.size(); ++i) {
        VradLightmapPage& page = result.pages[i];
        // Vulkan supports NPOT sampled images. Keeping the exact used extent
        // avoids promoting a sparse shelf page to (for example) a 4096² 128-MiB
        // image just because one chart reached the page's right edge. Mips are
        // deliberately disabled for the phase-one guttered lightmap path.
        page.width  = std::max(pages[i].usedWidth, 1u);
        page.height = std::max(pages[i].usedHeight, 1u);
        page.rgba16f.resize(static_cast<size_t>(page.width) * page.height * 4u, FloatToHalf(1.0f));
    }

    for (const PendingChart& source: charts) {
        const VradLightmapPage& pageInfo = result.pages[source.page];
        VradLightmapPage&       page     = result.pages[source.page];
        VradLightmapChart& destination  = result.faceCharts[source.faceIndex];
        destination.page   = source.page;
        destination.x      = source.outerX + gutter;
        destination.y      = source.outerY + gutter;
        destination.width  = source.width;
        destination.height = source.height;

        for (int32_t y = -static_cast<int32_t>(gutter); y < static_cast<int32_t>(source.height + gutter); ++y) {
            const uint32_t srcY = static_cast<uint32_t>(std::clamp(y, 0, static_cast<int32_t>(source.height) - 1));
            const uint32_t dstY = source.outerY + static_cast<uint32_t>(y + static_cast<int32_t>(gutter));
            for (int32_t x = -static_cast<int32_t>(gutter); x < static_cast<int32_t>(source.width + gutter); ++x) {
                const uint32_t srcX = static_cast<uint32_t>(std::clamp(x, 0, static_cast<int32_t>(source.width) - 1));
                const uint32_t dstX = source.outerX + static_cast<uint32_t>(x + static_cast<int32_t>(gutter));
                const auto rgb = DecodeVradLuxel(map.lighting[source.sourceLuxelOffset + srcY * source.width + srcX]);
                const size_t pixel = (static_cast<size_t>(dstY) * pageInfo.width + dstX) * 4u;
                page.rgba16f[pixel + 0u] = FloatToHalf(rgb[0]);
                page.rgba16f[pixel + 1u] = FloatToHalf(rgb[1]);
                page.rgba16f[pixel + 2u] = FloatToHalf(rgb[2]);
                page.rgba16f[pixel + 3u] = FloatToHalf(1.0f);
            }
        }
        ++result.faceCount;
    }

    return result;
}

} // namespace ZHLN::BSP

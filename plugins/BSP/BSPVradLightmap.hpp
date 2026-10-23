// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Native Source VRAD lightmaps.
//
// This is deliberately an importer-side data product rather than an AO map or
// a fitted lighting approximation. Each valid BSP face owns one chart in a
// linear RGBA16F atlas page, and the geometry importer maps the face's UV1
// stream directly into that chart. The renderer binds the resulting page as a
// dedicated baked-irradiance input.

#include "BSPRead.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::BSP {

inline constexpr uint32_t kInvalidVradLightmapPage = 0xFFFFFFFFu;

struct VradLightmapChart {
    uint32_t page   = kInvalidVradLightmapPage;
    uint32_t x      = 0; // Inner (non-gutter) texel origin in the page.
    uint32_t y      = 0;
    uint32_t width  = 0; // Luxels, including Source's final row/column.
    uint32_t height = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return page != kInvalidVradLightmapPage && width != 0 && height != 0;
    }
};

struct VradLightmapPage {
    uint32_t              width  = 0;
    uint32_t              height = 0;
    // IEEE-754 binary16 RGBA texels in linear space. Alpha is one; it exists
    // only because the renderer's generic bindless texture path is RGBA.
    std::vector<uint16_t> rgba16f;
};

struct VradLightmapAtlas {
    std::vector<VradLightmapPage> pages;
    // Indexed by BSPMap::faces. Invalid for faces with no static VRAD chart,
    // malformed offsets, or charts too large for the configured page size.
    std::vector<VradLightmapChart> faceCharts;

    uint32_t luxelCount       = 0;
    uint32_t faceCount        = 0;
    uint32_t rejectedFaceCount = 0;
    bool     hdrSource        = false;

    [[nodiscard]] const VradLightmapChart* ChartForFace(size_t faceIndex) const noexcept {
        if (faceIndex >= faceCharts.size() || !faceCharts[faceIndex]) {
            return nullptr;
        }
        return &faceCharts[faceIndex];
    }
};

// Decode Source's ColorRGBExp32 losslessly into the same linear radiance that
// VRAD authored. The caller is responsible for display exposure/tone mapping;
// no importer-wide calibration gain is applied here.
[[nodiscard]] auto DecodeVradLuxel(ColorRGBExp32 luxel) noexcept -> std::array<float, 3>;

// Packs each first-style face chart into page-size-bounded RGBA16F atlases.
// `gutter` replicated edge luxels protect bilinear sampling at chart seams.
// Pages use exact NPOT used extents; phase one samples mip zero so no sparse
// page is inflated merely to manufacture a mip chain.
// Secondary light styles are intentionally retained in BSPMap for the future
// animated-light-style stage; phase one uses Source's base (styles[0]) chart.
[[nodiscard]] auto BuildVradLightmapAtlas(const BSPMap& map, uint32_t maxPageSize, uint32_t gutter = 2u) -> VradLightmapAtlas;

} // namespace ZHLN::BSP

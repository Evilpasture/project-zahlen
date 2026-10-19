// project-zahlen -- BSP plugin
//
// BSPBakeField: legacy VRAD approximation fallback.
//
// Native BSP imports now preserve LUMP_LIGHTING[_HDR] as per-face sampled
// RGBA16F lightmap charts (BSPVradLightmap). That is the default and is the
// only path allowed when Source-fidelity static lighting is enabled.
//
// This file remains solely for maps/callers that explicitly opt out of native
// lightmaps and request the former ambient-plus-virtual-point-light fit. It is
// intentionally mutually exclusive with native atlas import: using both
// representations of one VRAD bake would double-count direct and indirect
// illumination and retain the clustered/RTGI work the native route removes.
//
// The approximation decodes the bake into:
//   1. an AmbientTerm, and
//   2. a budgeted set of virtual point lights.
//
// Do not route a native lightmap through this code or apply its calibration
// gain to a sampled authored luxel.
#pragma once

#include "BSPGeometry.hpp"
#include "BSPRead.hpp"

#include <cstdint>
#include <vector>

namespace ZHLN::BSP {

// The radiance field fitted from LUMP_LIGHTING. `ambient` feeds the SH /
// probe ambient path exactly like authored ambient; `vpls` join
// `imported.lights` exactly like authored point lights.
struct BakeField {
    AmbientTerm              ambient {};
    std::vector<BspPointLight> vpls {};
    bool                     hasBake = false;

    // Bookkeeping for harness runs and console reporting; zero-cost to the
    // shading path.
    uint32_t sampleCount = 0;
    float    totalArea   = 0.0F;
    float    peakRadiance = 0.0F;
};

// Fit the field. Conversion goes through the importer's own
// ConvertPosition/ConvertDirection so the field lands in engine space without
// duplicating the axis rules. `vplWeight` damps bake VPLs against the map's
// real lights + DDGI (the bake's direct image is already carried live),
// mirroring EmissiveLightFor's fixtureDamping convention.
[[nodiscard]] auto BuildBakeField(const BSPMap& map, const ImportOptions& options, uint32_t vplBudget, float vplWeight)
    -> BakeField;

} // namespace ZHLN::BSP

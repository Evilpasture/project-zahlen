// project-zahlen -- BSP plugin
//
// BSPBakeField: VRAD bake ingestion as a radiance field.
//
// The 2004 answer to LUMP_LIGHTING is to multiply it over lit surfaces as a
// texture. This pipeline has real lighting (PBR direct + SH ambient + DDGI
// radiance probes + RT shadows), and a lightmap multiply would fight all of
// it: it double-counts every direct light, and cannot inform surfaces that
// only the path tracer sees. So the bake is decoded once, off-line, and
// *fitted* into the engine's own light carriers instead:
//
//   1. an AmbientTerm -- the area-weighted mean radiance of the bake's FILL
//      population (faces below the area-weighted p60 luma: indirect/sky
//      bounce), encoded in the same irradiance = luma(color) x intensity
//      convention that the scene light's ambient ladder already uses (and
//      that frame.sh / the DDGI probe start state consume), and
//   2. a set of virtual point lights -- compact voxel clusters of the
//      bake's BRIGHT population (the direct image: sun and lamp bounce,
//      which the real lights + DDGI already carry live, so it is fitted at
//      reduced weight rather than folded into ambient), emitted as
//      ordinary BspPointLights so the existing shading path shades them
//      physically.
//
// What the bake cannot carry (view- and bounce-accurate direct occlusion)
// is left to the real lights and ray tracing rather than faked twice.
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

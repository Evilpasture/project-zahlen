// project-zahlen -- BSP plugin
//
// BSPBakeField: fitting VRAD's luxel radiance into the engine's carriers.
// See BSPBakeField.hpp for the design statement. Implementation outline:
//
//   decode  -> per-face mean radiance (style-0 block, area-weighted)
//   split   -> fill population (luma <= area-weighted p60) becomes the
//              AmbientTerm; the bright population is the "direct image"
//              (sun/lamp bounce the real lights + DDGI already carry live)
//              and is clustered into VPLs at reduced weight instead of being
//              folded into ambient (which would double-count it).
//   cluster -> 2 m voxels over engine-space face centroids; each cell's
//              energy goes through exactly EmissiveLightFor's brightness
//              model (kEmissiveSourceBrightnessPerSquareMetre,
//              kSourceBrightnessToEngine, kEmissiveMaxLightIntensity,
//              kEmissiveRangeScale clamps) so a bake cluster and an emissive
//              proxy are indistinguishable downstream.
//
// The bake's spatial detail finer than the voxel size is occlusion detail,
// which this pipeline resolves live with ray tracing -- not something to
// fake twice.

#include "BSPBakeField.hpp"

#include "BSPLighting.hpp"
#include "VMTParser.hpp"

#include <Zahlen/Core/Math.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ZHLN::BSP {
namespace {

constexpr float kFillQuantile          = 0.60F;   // lower-luma population = fill/ambient
constexpr float kVoxelMetres           = 2.0F;    // VPL clustering cell
constexpr float kAmbientReferenceLuma  = 0.34F;   // authored-ambient color luma convention
constexpr float kUnitsToMetres         = 0.0254F;
constexpr float kPi                    = 3.14159265358979323846F; // file-local, as BSPLighting.cpp

struct FaceRadiance {
    JPH::Vec3 positionEngine {0.0F, 0.0F, 0.0F}; // converted centroid
    float     radiance[3] {0.0F, 0.0F, 0.0F};    // mean luxel radiance (source-brightness family)
    float     luma        = 0.0F;
    float     areaSquareMetres = 0.0F;
};

// Newell's method: robust normal/area/centroid for the face's edge loop,
// concave polys included, in Source space (the lightmap UV basis lives there).
struct FacePolygon {
    bool  valid = false;
    float normal[3] {0.0F, 0.0F, 0.0F};
    float centroid[3] {0.0F, 0.0F, 0.0F};
    float area = 0.0F;
};

[[nodiscard]] auto TraceFacePolygon(const BSPMap& map, const DFace& face) -> FacePolygon {
    FacePolygon out;
    if (face.numedges < 3 || face.firstedge < 0 || static_cast<size_t>(face.firstedge) + static_cast<size_t>(face.numedges) > map.surfEdges.size()) {
        return out;
    }
    float newell[3] {0.0F, 0.0F, 0.0F};
    float sum[3] {0.0F, 0.0F, 0.0F};
    int32_t count = 0;
    for (int32_t i = 0; i < face.numedges; ++i) {
        const int32_t surfEdge = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>(i)];
        const int32_t edgeIndex = surfEdge >= 0 ? surfEdge : -surfEdge;
        if (edgeIndex < 0 || static_cast<size_t>(edgeIndex) >= map.edges.size()) {
            return out;
        }
        const DEdge& edge = map.edges[static_cast<size_t>(edgeIndex)];
        const int32_t vi   = surfEdge >= 0 ? edge.v[0] : edge.v[1];
        if (vi < 0 || static_cast<size_t>(vi) >= map.vertices.size()) {
            return out;
        }
        const DVertex& v = map.vertices[static_cast<size_t>(vi)];
        sum[0] += v.point[0];
        sum[1] += v.point[1];
        sum[2] += v.point[2];
        ++count;
    }
    if (count < 3) {
        return out;
    }
    // Second pass with the loop closed against the mean point for stability.
    for (int32_t i = 0; i < face.numedges; ++i) {
        const int32_t surfEdge = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>(i)];
        const int32_t edgeIndex = surfEdge >= 0 ? surfEdge : -surfEdge;
        const DEdge& edge = map.edges[static_cast<size_t>(edgeIndex)];
        const int32_t vi   = surfEdge >= 0 ? edge.v[0] : edge.v[1];
        const DVertex& a = map.vertices[static_cast<size_t>(vi)];

        const int32_t nextSurf = map.surfEdges[static_cast<size_t>(face.firstedge) + static_cast<size_t>((i + 1) % face.numedges)];
        const int32_t nextEdgeIndex = nextSurf >= 0 ? nextSurf : -nextSurf;
        const DEdge& nextEdge = map.edges[static_cast<size_t>(nextEdgeIndex)];
        const int32_t nextVi   = nextSurf >= 0 ? nextEdge.v[0] : nextEdge.v[1];
        const DVertex& b = map.vertices[static_cast<size_t>(nextVi)];

        newell[0] += (a.point[1] - b.point[1]) * (a.point[2] + b.point[2]);
        newell[1] += (a.point[2] - b.point[2]) * (a.point[0] + b.point[0]);
        newell[2] += (a.point[0] - b.point[0]) * (a.point[1] + b.point[1]);
    }
    const float length = std::sqrt(newell[0] * newell[0] + newell[1] * newell[1] + newell[2] * newell[2]);
    if (!(length > 1e-6F)) {
        return out;
    }
    out.valid = true;
    out.area  = 0.5F * length;
    out.normal[0] = newell[0] / length;
    out.normal[1] = newell[1] / length;
    out.normal[2] = newell[2] / length;
    out.centroid[0] = sum[0] / static_cast<float>(count);
    out.centroid[1] = sum[1] / static_cast<float>(count);
    out.centroid[2] = sum[2] / static_cast<float>(count);
    return out;
}

} // namespace

auto BuildBakeField(const BSPMap& map, const ImportOptions& options, uint32_t vplBudget, float vplWeight) -> BakeField {
    BakeField field;

    if (map.lighting.empty() || map.faces.empty()) {
        return field;
    }

    // ---------------------------------------------------------------------
    // 1. Decode: style-0 luxels -> per-face mean radiance.
    // ---------------------------------------------------------------------
    std::vector<FaceRadiance> faces;
    faces.reserve(map.faces.size());

    for (const DFace& face: map.faces) {
        if (face.lightofs < 0 || face.texinfo < 0 || static_cast<size_t>(face.texinfo) >= map.texInfos.size()) {
            continue;
        }
        const int32_t w = face.lightmapTextureSizeInLuxels[0];
        const int32_t h = face.lightmapTextureSizeInLuxels[1];
        if (w < 0 || h < 0) {
            continue;
        }
        const uint32_t sampleCount = static_cast<uint32_t>(w + 1) * static_cast<uint32_t>(h + 1);
        const size_t   firstSample = static_cast<size_t>(face.lightofs);
        if (firstSample + sampleCount > map.lighting.size()) {
            continue;
        }

        const FacePolygon poly = TraceFacePolygon(map, face);
        if (!poly.valid) {
            continue;
        }

        float sum[3] {0.0F, 0.0F, 0.0F};
        for (uint32_t s = 0; s < sampleCount; ++s) {
            const ColorRGBExp32& sample = map.lighting[firstSample + s];
            const float scale = std::exp2(static_cast<float>(sample.exponent));
            sum[0] += static_cast<float>(sample.r) * scale;
            sum[1] += static_cast<float>(sample.g) * scale;
            sum[2] += static_cast<float>(sample.b) * scale;
        }

        FaceRadiance entry;
        entry.radiance[0] = sum[0] / static_cast<float>(sampleCount);
        entry.radiance[1] = sum[1] / static_cast<float>(sampleCount);
        entry.radiance[2] = sum[2] / static_cast<float>(sampleCount);
        entry.luma        = Rec709Luma(JPH::Vec3 {entry.radiance[0], entry.radiance[1], entry.radiance[2]});
        entry.areaSquareMetres = poly.area * kUnitsToMetres * kUnitsToMetres;

        const JPH::Vec3 centroidSource {poly.centroid[0], poly.centroid[1], poly.centroid[2]};
        entry.positionEngine = ConvertPosition(options, centroidSource);

        faces.push_back(entry);
        field.sampleCount += sampleCount;
        field.totalArea += entry.areaSquareMetres;
        field.peakRadiance = std::max(field.peakRadiance, entry.luma);
    }

    if (faces.empty()) {
        return field;
    }
    field.hasBake = true;

    // ---------------------------------------------------------------------
    // 2. Split: fill population -> ambient, bright population -> VPLs.
    //    The boundary is the area-weighted p60 luma quantile.
    // ---------------------------------------------------------------------
    std::vector<FaceRadiance> sorted = faces;
    std::sort(sorted.begin(), sorted.end(), [](const FaceRadiance& a, const FaceRadiance& b) { return a.luma < b.luma; });
    float totalArea = 0.0F;
    for (const FaceRadiance& f: sorted) {
        totalArea += f.areaSquareMetres;
    }
    float        walkedArea = 0.0F;
    float        fillCeiling = sorted.back().luma;
    for (const FaceRadiance& f: sorted) {
        walkedArea += f.areaSquareMetres;
        if (walkedArea >= kFillQuantile * totalArea) {
            fillCeiling = f.luma;
            break;
        }
    }

    float fillWeight = 0.0F;
    float fillSum[3] {0.0F, 0.0F, 0.0F};
    for (const FaceRadiance& f: faces) {
        if (f.luma > fillCeiling) {
            continue;
        }
        fillWeight += f.areaSquareMetres;
        fillSum[0] += f.radiance[0] * f.areaSquareMetres;
        fillSum[1] += f.radiance[1] * f.areaSquareMetres;
        fillSum[2] += f.radiance[2] * f.areaSquareMetres;
    }
    if (fillWeight <= 0.0F) {
        // Everything is bright: ambient still deserves the global mean so the
        // fit degrades to "ambient-only" rather than nothing.
        for (const FaceRadiance& f: faces) {
            fillWeight += f.areaSquareMetres;
            fillSum[0] += f.radiance[0] * f.areaSquareMetres;
            fillSum[1] += f.radiance[1] * f.areaSquareMetres;
            fillSum[2] += f.radiance[2] * f.areaSquareMetres;
        }
    }

    const float fillRadiance[3] {fillSum[0] / fillWeight, fillSum[1] / fillWeight, fillSum[2] / fillWeight};
    const float fillLuma = std::max(Rec709Luma(JPH::Vec3 {fillRadiance[0], fillRadiance[1], fillRadiance[2]}), 1e-4F);

    // AmbientTerm convention (BSPLighting ladder): irradiance =
    // luma(color) * intensity / kAmbientBrightnessLadder. Carry the fit's
    // irradiance exactly, hue in color at the authored luma of 0.34.
    field.ambient.color     = JPH::Vec3 {fillRadiance[0] / fillLuma * kAmbientReferenceLuma,
                                     fillRadiance[1] / fillLuma * kAmbientReferenceLuma,
                                     fillRadiance[2] / fillLuma * kAmbientReferenceLuma};
    field.ambient.intensity = fillLuma * kAmbientBrightnessLadder / kAmbientReferenceLuma;
    field.ambient.authored  = false;

    // ---------------------------------------------------------------------
    // 3. Bright population -> voxel clusters -> VPLs via the emissive model.
    // ---------------------------------------------------------------------
    struct Cluster {
        JPH::Vec3 centroidSum {0.0F, 0.0F, 0.0F};
        float     radianceSum[3] {0.0F, 0.0F, 0.0F};
        float     areaSum = 0.0F;
        float     weight  = 0.0F;
    };
    std::unordered_map<uint64_t, Cluster> clusters;
    clusters.reserve(faces.size() / 4 + 1);

    const auto cellKey = [](const JPH::Vec3& p) -> uint64_t {
        const int64_t ix = static_cast<int64_t>(std::floor(p.GetX() / kVoxelMetres));
        const int64_t iy = static_cast<int64_t>(std::floor(p.GetY() / kVoxelMetres));
        const int64_t iz = static_cast<int64_t>(std::floor(p.GetZ() / kVoxelMetres));
        return (static_cast<uint64_t>(ix & 0x1FFFFF) << 42) | (static_cast<uint64_t>(iy & 0x1FFFFF) << 21) | static_cast<uint64_t>(iz & 0x1FFFFF);
    };

    for (const FaceRadiance& f: faces) {
        if (f.luma <= fillCeiling || f.areaSquareMetres <= 0.0F) {
            continue;
        }
        Cluster& cluster = clusters[cellKey(f.positionEngine)];
        const float w = f.areaSquareMetres;
        cluster.centroidSum += f.positionEngine * w;
        cluster.radianceSum[0] += f.radiance[0] * w;
        cluster.radianceSum[1] += f.radiance[1] * w;
        cluster.radianceSum[2] += f.radiance[2] * w;
        cluster.areaSum += w;
        cluster.weight += 1.0F;
    }

    std::vector<BspPointLight> vpls;
    vpls.reserve(clusters.size());
    for (const auto& [key, cluster]: clusters) {
        if (cluster.areaSum <= 0.0F) {
            continue;
        }
        const JPH::Vec3 position  = cluster.centroidSum / cluster.areaSum;
        const JPH::Vec3 radiance {cluster.radianceSum[0] / cluster.areaSum, cluster.radianceSum[1] / cluster.areaSum,
                                  cluster.radianceSum[2] / cluster.areaSum};

        BspPointLight light;
        light.name       = "bake_vpl";
        light.className  = "bake_vpl";
        light.position   = position;
        light.color      = radiance;
        light.fromEmissive = false; // fitted from the bake, not an emissive surface

        // Exactly EmissiveLightFor's equivalent-Source-brightness chain, with
        // the cluster radiance standing in for surface luminance and
        // bakeVplWeight standing in for the fixture damping (the map's real
        // lights + DDGI carry the direct image; the VPL is the bake's echo of
        // it).
        const float equivalentBrightness =
            Rec709Luma(radiance) * cluster.areaSum * kEmissiveSourceBrightnessPerSquareMetre * vplWeight;
        light.intensity = std::max(equivalentBrightness * kSourceBrightnessToEngine, 0.0F);
        if (light.intensity > kEmissiveMaxLightIntensity) {
            light.intensity = kEmissiveMaxLightIntensity;
        }
        const float characteristic = std::sqrt(std::max(cluster.areaSum, 1e-4F) / kPi);
        light.range                = std::clamp(characteristic * kEmissiveRangeScale, kMinRangeMetres, kMaxRangeMetres);
        light.radius               = std::clamp(characteristic, 0.02F, 2.0F);
        light.castShadows          = false;
        light.startsOn             = light.intensity > 0.0F;
        if (light.startsOn) {
            vpls.push_back(light);
        }
    }

    std::sort(vpls.begin(), vpls.end(),
              [](const BspPointLight& a, const BspPointLight& b) { return a.intensity > b.intensity; });
    if (vpls.size() > vplBudget) {
        vpls.resize(vplBudget);
    }
    field.vpls = std::move(vpls);
    return field;
}

} // namespace ZHLN::BSP

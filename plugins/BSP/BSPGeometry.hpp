// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPGeometry.hpp
//
// The import step: BSP lumps in, Zahlen-native vertex streams out. The
// streams are exactly the cooked-mesh payload layout the renderer already
// consumes -- VertexPosition / VertexTangentFrame / VertexSurface / indices
// (see <Zahlen/Vertex.hpp>) -- grouped per material the way the glTF importer
// groups primitives. Nothing here touches the GPU, the ECS, or an instantiator;
// the importer adapter uploads these streams and Scene::Instantiate /
// PrefabFactory::InstantiatePrefab take it from there.
//
// Engine-free by design: this layer is buildable without the full renderer tree.

#include "BSPRead.hpp"
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Vertex.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN::BSP {

// Source is Z-up and measures in inches; the engine is Y-up (glTF convention)
// and measures in metres. The default converts; unitScale lets a caller keep
// inches (1.0f) or feed a differently scaled world.
struct ImportOptions {
    bool        convertCoordinates   = true;
    float       unitScale            = 0.0254f; // Inches to meters
    bool        includeDisplacements = true;
    bool        gatherLights         = true;
    bool        buildColliders       = true;
    std::string assetRoot;
    std::string lightmapAtlasPath;
};

// One material group: the concatenated, de-indexed-then-reindexed streams of
// every face that uses one texdata name.
struct MaterialStreams {
    std::string materialName;

    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> tangentFrames;
    std::vector<VertexSurface>      surfaces;
    std::vector<uint32_t>           indices;

    JPH::Float3 boundsMin {0.0f, 0.0f, 0.0f};
    JPH::Float3 boundsMax {0.0f, 0.0f, 0.0f};

    [[nodiscard]] auto VertexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(positions.size());
    }
    [[nodiscard]] auto IndexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(indices.size());
    }
};

// A light recovered from the entity lump.
struct BspPointLight {
    std::string name;
    LightType   type = LightType::Point; // Direct reuse of engine enum

    JPH::Vec3 position {0.0f, 0.0f, 0.0f};
    JPH::Vec3 direction {0.0f, -1.0f, 0.0f};
    JPH::Vec3 color {1.0f, 1.0f, 1.0f};
    float     intensity        = 1.0f;
    float     innerConeRadians = 0.0f;
    float     outerConeRadians = 0.78539816339f; // Default 45 deg
};

struct ImportedMapData {
    std::vector<MaterialStreams> parts;
    std::vector<BspPointLight>   lights;

    JPH::Float3 boundsMin {0.0f, 0.0f, 0.0f};
    JPH::Float3 boundsMax {0.0f, 0.0f, 0.0f};
    uint32_t    faceCount         = 0;
    uint32_t    displacementCount = 0;
    uint32_t    triangleCount     = 0;
};

// Position transform for the options in force (Source Z-up inches -> engine
// Y-up metres by default). Exposed so scene importing converts identically.
[[nodiscard]] auto ConvertPosition(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;
[[nodiscard]] auto ConvertDirection(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;

// Faces + displacements -> per-material native streams; entities -> lights.
// Faces whose texinfo is missing or node-only are skipped (they are tree
// partitions, not surfaces). Degenerate polygons (fewer than 3 unique corners)
// are skipped rather than producing zero-area triangles.
[[nodiscard]] auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options = {}) -> ImportedMapData;

} // namespace ZHLN::BSP

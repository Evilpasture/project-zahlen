// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPGeometry.hpp
//
// The marshalling step: BSP lumps in, Zahlen-native vertex streams out. The
// streams are exactly the cooked-mesh payload layout the renderer already
// consumes -- VertexPosition / VertexTangentFrame / VertexSurface / indices
// (see <Zahlen/Vertex.hpp>) -- grouped per material the way the glTF importer
// groups primitives. Nothing here touches the GPU, the ECS, or an instantiator;
// the importer adapter uploads these streams and Scene::Instantiate /
// PrefabFactory::InstantiatePrefab take it from there.
//
// Engine-free by design: this is the layer the standalone calculator builds.

#include "BSPRead.hpp"
#include "BSPTypes.hpp"

#include <Zahlen/Math3D.hpp>
#include <Zahlen/Vertex.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN::BSP {

// Source is Z-up and measures in inches; the engine is Y-up (glTF convention)
// and measures in metres. The default converts; unitScale lets a caller keep
// inches (1.0f) or feed a differently scaled world.
struct MarshallOptions {
    bool  convertCoordinates   = true;
    float unitScale            = 0.0254f;
    bool  includeDisplacements = true;
    bool  gatherLights         = true;
};

// One material group: the concatenated, de-indexed-then-reindexed streams of
// every face that uses one texdata name.
struct MaterialStreams {
    std::string materialName;

    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> tangentFrames;
    std::vector<VertexSurface>      surfaces;
    std::vector<uint32_t>           indices;

    float boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float boundsMax[3] = {0.0f, 0.0f, 0.0f};

    [[nodiscard]] auto VertexCount() const noexcept -> uint32_t { return static_cast<uint32_t>(positions.size()); }
    [[nodiscard]] auto IndexCount() const noexcept -> uint32_t { return static_cast<uint32_t>(indices.size()); }
};

// A light recovered from the entity lump. Carries no engine types so the
// marshaller stays buildable standalone; the adapter converts to ModelLight.
struct BspPointLight {
    enum class Kind : uint8_t { Point, Spot, Directional };

    std::string name;
    Kind        kind = Kind::Point;

    float position[3] = {0.0f, 0.0f, 0.0f};
    float direction[3] = {0.0f, -1.0f, 0.0f}; // spot/environment aim, converted space
    float color[3]    = {1.0f, 1.0f, 1.0f};
    float intensity   = 1.0f; // the "_light" alpha channel, as authored
    float innerConeRadians = 0.0f;
    float outerConeRadians = 0.78539816339f;
};

struct MarshalledMap {
    std::vector<MaterialStreams> parts;
    std::vector<BspPointLight>   lights;

    float    boundsMin[3] = {0.0f, 0.0f, 0.0f};
    float    boundsMax[3] = {0.0f, 0.0f, 0.0f};
    uint32_t faceCount        = 0;
    uint32_t displacementCount = 0;
    uint32_t triangleCount    = 0;
};

// Position transform for the options in force (Source Z-up inches -> engine
// Y-up metres by default). Exposed so the scene marshalling and the
// calculator convert identically.
void ConvertPosition(const MarshallOptions& options, const float in[3], float out[3]) noexcept;
void ConvertDirection(const MarshallOptions& options, const float in[3], float out[3]) noexcept;

// Faces + displacements -> per-material native streams; entities -> lights.
// Faces whose texinfo is missing or node-only are skipped (they are tree
// partitions, not surfaces). Degenerate polygons (fewer than 3 unique corners)
// are skipped rather than producing zero-area triangles.
[[nodiscard]] auto MarshallMap(const BSPMap& map, const MarshallOptions& options = {}) -> MarshalledMap;

} // namespace ZHLN::BSP

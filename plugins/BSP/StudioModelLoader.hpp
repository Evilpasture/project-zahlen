// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BSPGeometry.hpp"
#include "StudioModelTypes.hpp"
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Vertex.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::BSP {

struct StudioMeshPart {
    std::string                     materialName;
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> tangentFrames;
    std::vector<VertexSurface>      surfaces;
    std::vector<VertexSkin>         skinWeights;
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

struct StudioPhysicsConvex {
    std::vector<JPH::Vec3> vertices;
};

struct StudioPhysicsSolid {
    std::vector<StudioPhysicsConvex> convexHulls;
};

struct StudioModelData {
    std::string                     name;
    int32_t                         version = 0;
    int32_t                         checksum = 0;
    JPH::Float3                     hullMin {0.0f, 0.0f, 0.0f};
    JPH::Float3                     hullMax {0.0f, 0.0f, 0.0f};
    std::vector<std::string>        textureSearchPaths; // cdtextures
    std::vector<std::string>        textureNames;       // textures
    std::vector<std::string>        materialNames;      // combined search paths for each texture index
    std::vector<StudioMeshPart>     parts;
    std::vector<StudioPhysicsSolid> physicsSolids;
};

// Parses StudioModel files (.mdl, .vvd, .dx90.vtx/.vtx, optional .phy)
// from in-memory byte buffers into structured, engine-free vertex/mesh streams.
[[nodiscard]] auto ParseStudioModel(
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes = {},
    const ImportOptions&       options  = {}
) -> std::expected<StudioModelData, ErrorCode>;

} // namespace ZHLN::BSP

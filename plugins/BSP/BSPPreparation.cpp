// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "BSPPreparation.hpp"
#include "BSPLighting.hpp"
#include "VMTLighting.hpp"
#include <Zahlen/Render/Vertex.hpp>
#include <algorithm>
#include <string>
#include <unordered_map>
namespace ZHLN::BSP {
auto LoadMaterialVmt(const SourceVFS& vfs, std::string_view materialName) -> std::optional<VMTMaterial> {
    const auto vmtPath = vfs.ResolveMaterial(materialName);
    if (!vmtPath.has_value()) {
        return std::nullopt;
    }
    const auto vmtBytes = vfs.ReadFile(*vmtPath);
    if (!vmtBytes.has_value() || vmtBytes->empty()) {
        return std::nullopt;
    }
    const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
    const auto             resolved = ResolveVmtMaterial(vmtText, [&vfs](std::string_view includePath) -> std::optional<std::string> {
        const auto includeBytes = vfs.ReadFile(std::string(includePath));
        if (!includeBytes.has_value() || includeBytes->empty()) {
            return std::nullopt;
        }
        return std::string(reinterpret_cast<const char*>(includeBytes->data()), includeBytes->size());
    });
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    return std::optional<VMTMaterial>(std::move(*resolved));
}

// The second stage of the emissive heuristic: the geometry pass only applied
// the material-name gate (it has no VFS), so this is where the evidence that
// actually decides the strength is read -- the .vmt's shader class and
// self-illumination keys, the measured luminance of the base .vtf, and the
// texdata reflectivity vbsp baked. Every candidate is re-probed through
// BSPLighting::ProbeEmissiveMaterial, and the ones that clear no threshold are
// dropped: a `lights/white` sheet with reflectivity (1,1,1) and no
// self-illumination is a lit surface, not a lamp, and spawning a light at its
// centroid would double the map's illumination.
//
// Runs before the prefab is built so the parts' `unlit` flag and the spawned
// lights agree on which surfaces are emitters.
auto RefineEmissiveSurfaces(const SourceVFS& vfs, std::span<EmissiveSurface> surfaces, ImportedMapData& data) -> std::vector<EmissiveSurface> {
    if (surfaces.empty()) {
        return {};
    }
    std::unordered_map<std::string, std::optional<VMTMaterial>> vmtCache;
    std::vector<EmissiveSurface>                                kept;
    kept.reserve(surfaces.size());

    for (EmissiveSurface& surface: surfaces) {
        const auto vmtIt = vmtCache.try_emplace(surface.materialName, std::nullopt);
        if (vmtIt.second) {
            vmtIt.first->second = LoadMaterialVmt(vfs, surface.materialName);
        }
        const VMTMaterial* vmt = vmtIt.first->second.has_value() ? &*vmtIt.first->second : nullptr;

        // Measured luminance of the material's own base texture: the most
        // direct evidence a surface emits, and it overrides the family name
        // when the .vmt declares no self-illumination of its own.
        const float luma = (vmt != nullptr && !vmt->baseTexture.empty()) ? MeasureVmtTextureLuma(vfs, *vmt) : -1.0f;

        const EmissiveMaterialProbe probe = ProbeEmissiveMaterial(surface.materialName, surface.reflectivityLuma, vmt, surface.noLightFlag);
        if (!probe.isEmissive || probe.strength <= 0.0f) {
            continue;
        }
        surface.strength = std::max(probe.strength, (luma > 0.0f) ? luma * 0.85f : 0.0f);
        surface.color    = probe.tint;
        if (surface.reflectivityLuma < 0.0f) {
            surface.reflectivityLuma = 0.0f;
        }
        kept.push_back(std::move(surface));
    }

    data.stats.emissiveFaces = 0;
    for (const EmissiveSurface& surface: kept) {
        data.stats.emissiveFaces += surface.faceCount;
    }
    data.stats.emissiveSurfaces = static_cast<uint32_t>(kept.size());
    return kept;
}

// Translates every stream (render parts and the collision-only fold) by
// offset. Used to re-base rotating doors on their hinge pivot so the entity
// transform -- placed at the pivot -- rotates the geometry about it.
void ShiftImportedGeometry(ImportedMapData& data, FVec3 offset) {
    const auto shiftStreams = [offset](MaterialStreams& streams) {
        for (VertexPosition& vertex: streams.positions) {
            vertex.position[0] += offset.x;
            vertex.position[1] += offset.y;
            vertex.position[2] += offset.z;
        }
        streams.boundsMin.x += offset.x;
        streams.boundsMin.y += offset.y;
        streams.boundsMin.z += offset.z;
        streams.boundsMax.x += offset.x;
        streams.boundsMax.y += offset.y;
        streams.boundsMax.z += offset.z;
    };
    for (MaterialStreams& part: data.parts) {
        shiftStreams(part);
    }
    shiftStreams(data.collisionOnly);
    data.boundsMin.x += offset.x;
    data.boundsMin.y += offset.y;
    data.boundsMin.z += offset.z;
    data.boundsMax.x += offset.x;
    data.boundsMax.y += offset.y;
    data.boundsMax.z += offset.z;
}

} // namespace ZHLN::BSP

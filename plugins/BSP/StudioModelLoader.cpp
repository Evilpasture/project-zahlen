// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "StudioModelLoader.hpp"
#include "BSPRead.hpp"
#include <algorithm>
#include <cstring>

namespace ZHLN::BSP {

auto ParseStudioModel(
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes,
    const ImportOptions&       options
) -> std::expected<StudioModelData, ErrorCode> {
    // 1. Validate & Parse .MDL Header
    if (mdlBytes.size() < sizeof(StudioHdr)) {
        return std::unexpected(BSPError::FileTooSmall);
    }

    StudioHdr hdr {};
    std::memcpy(&hdr, mdlBytes.data(), sizeof(StudioHdr));

    if (hdr.id != kStudioHdrMagic) {
        return std::unexpected(BSPError::InvalidIdent);
    }
    if (hdr.version < 44 || hdr.version > 53) {
        return std::unexpected(BSPError::UnsupportedVersion);
    }

    // 2. Validate & Parse .VVD Header
    if (vvdBytes.size() < sizeof(VvdHeader)) {
        return std::unexpected(BSPError::FileTooSmall);
    }

    VvdHeader vvd {};
    std::memcpy(&vvd, vvdBytes.data(), sizeof(VvdHeader));

    if (vvd.id != kVvdMagic) {
        return std::unexpected(BSPError::InvalidIdent);
    }
    if (vvd.checksum != hdr.checksum) {
        return std::unexpected(BSPError::ChecksumMismatch);
    }

    // Process .VVD vertices for LOD 0
    std::vector<StudioVertex> lod0Vertices;
    const size_t              targetLOD         = 0;
    const int32_t             numTargetLODVerts = vvd.numLODVertexes[targetLOD];

    if (numTargetLODVerts > 0) {
        if (vvd.numFixups == 0) {
            if (vvd.vertexDataStart < 0 ||
                static_cast<size_t>(vvd.vertexDataStart) + static_cast<size_t>(numTargetLODVerts) * sizeof(StudioVertex) > vvdBytes.size()) {
                return std::unexpected(BSPError::ShortRead);
            }
            lod0Vertices.resize(static_cast<size_t>(numTargetLODVerts));
            std::memcpy(lod0Vertices.data(), vvdBytes.data() + vvd.vertexDataStart, static_cast<size_t>(numTargetLODVerts) * sizeof(StudioVertex));
        } else {
            if (vvd.fixupTableStart < 0 ||
                static_cast<size_t>(vvd.fixupTableStart) + static_cast<size_t>(vvd.numFixups) * sizeof(VvdFixup) > vvdBytes.size()) {
                return std::unexpected(BSPError::ShortRead);
            }

            const auto* fixups   = reinterpret_cast<const VvdFixup*>(vvdBytes.data() + vvd.fixupTableStart);
            const auto* rawVerts = reinterpret_cast<const StudioVertex*>(vvdBytes.data() + vvd.vertexDataStart);

            lod0Vertices.reserve(static_cast<size_t>(numTargetLODVerts));
            for (int32_t i = 0; i < vvd.numFixups; ++i) {
                const VvdFixup& fixup = fixups[i];
                if (fixup.lod >= static_cast<int32_t>(targetLOD)) {
                    if (static_cast<size_t>(vvd.vertexDataStart) + static_cast<size_t>(fixup.sourceVertexID + fixup.numVertexes) * sizeof(StudioVertex) > vvdBytes.size()) {
                        return std::unexpected(BSPError::ShortRead);
                    }
                    for (int32_t v = 0; v < fixup.numVertexes; ++v) {
                        lod0Vertices.push_back(rawVerts[fixup.sourceVertexID + v]);
                    }
                }
            }
        }
    }

    // 3. Validate & Parse .VTX Header
    if (vtxBytes.size() < sizeof(VtxHeader)) {
        return std::unexpected(BSPError::FileTooSmall);
    }

    VtxHeader vtx {};
    std::memcpy(&vtx, vtxBytes.data(), sizeof(VtxHeader));

    if (vtx.checkSum != hdr.checksum) {
        return std::unexpected(BSPError::ChecksumMismatch);
    }

    // 4. Populate Model Metadata & Textures
    StudioModelData modelData;
    modelData.name     = hdr.name;
    modelData.version  = hdr.version;
    modelData.checksum = hdr.checksum;

    const JPH::Vec3 minEngine = ConvertPosition(options, JPH::Vec3(hdr.hull_min[0], hdr.hull_min[1], hdr.hull_min[2]));
    const JPH::Vec3 maxEngine = ConvertPosition(options, JPH::Vec3(hdr.hull_max[0], hdr.hull_max[1], hdr.hull_max[2]));
    modelData.hullMin = {std::min(minEngine.GetX(), maxEngine.GetX()), std::min(minEngine.GetY(), maxEngine.GetY()), std::min(minEngine.GetZ(), maxEngine.GetZ())};
    modelData.hullMax = {std::max(minEngine.GetX(), maxEngine.GetX()), std::max(minEngine.GetY(), maxEngine.GetY()), std::max(minEngine.GetZ(), maxEngine.GetZ())};

    // Extract cdtextures (material search directories)
    if (hdr.numcdtextures > 0 && hdr.cdtextureindex > 0 && static_cast<size_t>(hdr.cdtextureindex) < mdlBytes.size()) {
        const auto* cdOffsets = reinterpret_cast<const int32_t*>(mdlBytes.data() + hdr.cdtextureindex);
        for (int32_t i = 0; i < hdr.numcdtextures; ++i) {
            const int32_t strOffset = cdOffsets[i];
            if (strOffset > 0 && static_cast<size_t>(strOffset) < mdlBytes.size()) {
                const char* str = reinterpret_cast<const char*>(mdlBytes.data() + strOffset);
                std::string path(str);
                std::replace(path.begin(), path.end(), '\\', '/');
                if (!path.empty() && path.back() != '/') {
                    path.push_back('/');
                }
                modelData.textureSearchPaths.push_back(std::move(path));
            }
        }
    }
    if (modelData.textureSearchPaths.empty()) {
        modelData.textureSearchPaths.emplace_back("");
    }

    // Extract textures & build primary material candidate paths
    if (hdr.numtextures > 0 && hdr.textureindex > 0 && static_cast<size_t>(hdr.textureindex) < mdlBytes.size()) {
        for (int32_t i = 0; i < hdr.numtextures; ++i) {
            const size_t texOffset = static_cast<size_t>(hdr.textureindex) + i * sizeof(StudioTexture);
            if (texOffset + sizeof(StudioTexture) <= mdlBytes.size()) {
                const auto* tex = reinterpret_cast<const StudioTexture*>(mdlBytes.data() + texOffset);
                const size_t nameOffset = texOffset + tex->sznameindex;
                if (nameOffset < mdlBytes.size()) {
                    const char* nameStr = reinterpret_cast<const char*>(mdlBytes.data() + nameOffset);
                    std::string tName(nameStr);
                    std::replace(tName.begin(), tName.end(), '\\', '/');
                    modelData.textureNames.push_back(tName);

                    std::string matPath = modelData.textureSearchPaths[0] + tName;
                    modelData.materialNames.push_back(std::move(matPath));
                }
            }
        }
    }

    // 5. Traverse BodyParts -> Models -> LOD 0 -> Meshes -> StripGroups
    if (hdr.numbodyparts > 0 && vtx.numBodyParts >= hdr.numbodyparts &&
        hdr.bodypartindex > 0 && vtx.bodyPartOffset > 0) {
        
        for (int32_t bpIdx = 0; bpIdx < hdr.numbodyparts; ++bpIdx) {
            const size_t mdlBpOfs = static_cast<size_t>(hdr.bodypartindex) + bpIdx * sizeof(StudioBodyPart);
            const size_t vtxBpOfs = static_cast<size_t>(vtx.bodyPartOffset) + bpIdx * sizeof(VtxBodyPart);

            if (mdlBpOfs + sizeof(StudioBodyPart) > mdlBytes.size() ||
                vtxBpOfs + sizeof(VtxBodyPart) > vtxBytes.size()) {
                continue;
            }

            const auto* mdlBp = reinterpret_cast<const StudioBodyPart*>(mdlBytes.data() + mdlBpOfs);
            const auto* vtxBp = reinterpret_cast<const VtxBodyPart*>(vtxBytes.data() + vtxBpOfs);

            const int32_t numModels = std::min(mdlBp->nummodels, vtxBp->numModels);
            for (int32_t mIdx = 0; mIdx < numModels; ++mIdx) {
                const size_t mdlModelOfs = mdlBpOfs + mdlBp->modelindex + mIdx * sizeof(StudioModel);
                const size_t vtxModelOfs = vtxBpOfs + vtxBp->modelOffset + mIdx * sizeof(VtxModel);

                if (mdlModelOfs + sizeof(StudioModel) > mdlBytes.size() ||
                    vtxModelOfs + sizeof(VtxModel) > vtxBytes.size()) {
                    continue;
                }

                const auto* mdlModel = reinterpret_cast<const StudioModel*>(mdlBytes.data() + mdlModelOfs);
                const auto* vtxModel = reinterpret_cast<const VtxModel*>(vtxBytes.data() + vtxModelOfs);

                if (vtxModel->numLODs <= 0) {
                    continue;
                }

                const size_t vtxLodOfs = vtxModelOfs + vtxModel->lodOffset + 0 * sizeof(VtxModelLOD);
                if (vtxLodOfs + sizeof(VtxModelLOD) > vtxBytes.size()) {
                    continue;
                }
                const auto* vtxLod = reinterpret_cast<const VtxModelLOD*>(vtxBytes.data() + vtxLodOfs);

                const int32_t numMeshes = std::min(mdlModel->nummeshes, vtxLod->numMeshes);
                for (int32_t meshIdx = 0; meshIdx < numMeshes; ++meshIdx) {
                    const size_t mdlMeshOfs = mdlModelOfs + mdlModel->meshindex + meshIdx * sizeof(StudioMesh);
                    const size_t vtxMeshOfs = vtxLodOfs + vtxLod->meshOffset + meshIdx * sizeof(VtxMesh);

                    if (mdlMeshOfs + sizeof(StudioMesh) > mdlBytes.size() ||
                        vtxMeshOfs + sizeof(VtxMesh) > vtxBytes.size()) {
                        continue;
                    }

                    const auto* mdlMesh = reinterpret_cast<const StudioMesh*>(mdlBytes.data() + mdlMeshOfs);
                    const auto* vtxMesh = reinterpret_cast<const VtxMesh*>(vtxBytes.data() + vtxMeshOfs);

                    StudioMeshPart part;
                    if (mdlMesh->material >= 0 && static_cast<size_t>(mdlMesh->material) < modelData.materialNames.size()) {
                        part.materialName = modelData.materialNames[mdlMesh->material];
                    } else if (mdlMesh->material >= 0 && static_cast<size_t>(mdlMesh->material) < modelData.textureNames.size()) {
                        part.materialName = modelData.textureNames[mdlMesh->material];
                    } else {
                        part.materialName = "default";
                    }

                    bool boundsFirst = true;

                    for (int32_t sgIdx = 0; sgIdx < vtxMesh->numStripGroups; ++sgIdx) {
                        const size_t vtxSgOfs = vtxMeshOfs + vtxMesh->stripGroupHeaderOffset + sgIdx * sizeof(VtxStripGroup);
                        if (vtxSgOfs + sizeof(VtxStripGroup) > vtxBytes.size()) {
                            continue;
                        }

                        const auto* vtxSg          = reinterpret_cast<const VtxStripGroup*>(vtxBytes.data() + vtxSgOfs);
                        const size_t vtxVertsOfs   = vtxSgOfs + vtxSg->vertOffset;
                        const size_t vtxIndicesOfs = vtxSgOfs + vtxSg->indexOffset;

                        if (vtxVertsOfs + vtxSg->numVerts * sizeof(VtxVertex) > vtxBytes.size() ||
                            vtxIndicesOfs + vtxSg->numIndices * sizeof(uint16_t) > vtxBytes.size()) {
                            continue;
                        }

                        const auto* sgVerts = reinterpret_cast<const VtxVertex*>(vtxBytes.data() + vtxVertsOfs);
                        auto getSgIndex = [&](size_t idx) -> uint16_t {
                            uint16_t val = 0;
                            std::memcpy(&val, vtxBytes.data() + vtxIndicesOfs + idx * sizeof(uint16_t), sizeof(uint16_t));
                            return val;
                        };

                        std::vector<uint32_t> sgToPartVert(vtxSg->numVerts, 0xFFFFFFFF);

                        auto getOrAddVertex = [&](uint16_t sgVertIdx) -> uint32_t {
                            if (sgVertIdx >= vtxSg->numVerts) {
                                return 0;
                            }
                            if (sgToPartVert[sgVertIdx] != 0xFFFFFFFF) {
                                return sgToPartVert[sgVertIdx];
                            }

                            const VtxVertex& vtxV           = sgVerts[sgVertIdx];
                            const size_t     globalVvdIdx   = static_cast<size_t>(vtxV.origMeshVertID) +
                                                          static_cast<size_t>(mdlMesh->vertexoffset) +
                                                          static_cast<size_t>(mdlModel->vertexindex);

                            StudioVertex sv {};
                            if (globalVvdIdx < lod0Vertices.size()) {
                                sv = lod0Vertices[globalVvdIdx];
                            }

                            const JPH::Vec3 rawPos(sv.pos[0], sv.pos[1], sv.pos[2]);
                            const JPH::Vec3 rawNorm(sv.normal[0], sv.normal[1], sv.normal[2]);

                            const JPH::Vec3 engPos  = ConvertPosition(options, rawPos);
                            const JPH::Vec3 engNorm = ConvertDirection(options, rawNorm);

                            const uint32_t newIndex = static_cast<uint32_t>(part.positions.size());

                            part.positions.push_back(VertexPosition {{engPos.GetX(), engPos.GetY(), engPos.GetZ()}});
                            part.tangentFrames.push_back(VertexTangentFrame {
                                Math::PackNormal(engNorm.GetX(), engNorm.GetY(), engNorm.GetZ()),
                                Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f)
                            });
                            part.surfaces.push_back(VertexSurface {
                                Math::PackUV(sv.texCoord[0], sv.texCoord[1]),
                                Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f),
                                Math::PackUV(0.0f, 0.0f)
                            });

                            VertexSkin skin {};
                            for (size_t b = 0; b < 3; ++b) {
                                skin.joints[b] = (b < vtxV.numBones && vtxV.boneID[b] >= 0) ? static_cast<uint16_t>(vtxV.boneID[b]) : 0;
                            }
                            skin.joints[3] = 0;
                            skin.weights   = Math::PackColor(sv.boneWeights.weight[0], sv.boneWeights.weight[1], sv.boneWeights.weight[2], 0.0f);
                            part.skinWeights.push_back(skin);

                            if (boundsFirst) {
                                part.boundsMin = {engPos.GetX(), engPos.GetY(), engPos.GetZ()};
                                part.boundsMax = {engPos.GetX(), engPos.GetY(), engPos.GetZ()};
                                boundsFirst    = false;
                            } else {
                                part.boundsMin.x = std::min(part.boundsMin.x, engPos.GetX());
                                part.boundsMin.y = std::min(part.boundsMin.y, engPos.GetY());
                                part.boundsMin.z = std::min(part.boundsMin.z, engPos.GetZ());
                                part.boundsMax.x = std::max(part.boundsMax.x, engPos.GetX());
                                part.boundsMax.y = std::max(part.boundsMax.y, engPos.GetY());
                                part.boundsMax.z = std::max(part.boundsMax.z, engPos.GetZ());
                            }

                            sgToPartVert[sgVertIdx] = newIndex;
                            return newIndex;
                        };

                        const size_t vtxStripsOfs = vtxSgOfs + vtxSg->stripOffset;
                        if (vtxStripsOfs + vtxSg->numStrips * sizeof(VtxStrip) <= vtxBytes.size()) {
                            const auto* strips = reinterpret_cast<const VtxStrip*>(vtxBytes.data() + vtxStripsOfs);
                            for (int32_t sIdx = 0; sIdx < vtxSg->numStrips; ++sIdx) {
                                const VtxStrip& strip = strips[sIdx];
                                if ((strip.flags & 0x02) != 0) {
                                    // Tristrip
                                    for (int32_t t = 0; t + 2 < strip.numIndices; ++t) {
                                        uint16_t i0 = getSgIndex(strip.indexOffset + t);
                                        uint16_t i1 = getSgIndex(strip.indexOffset + t + 1);
                                        uint16_t i2 = getSgIndex(strip.indexOffset + t + 2);
                                        if (i0 == i1 || i1 == i2 || i0 == i2) {
                                            continue;
                                        }
                                        if (t % 2 == 1) {
                                            std::swap(i0, i1);
                                        }
                                        part.indices.push_back(getOrAddVertex(i0));
                                        part.indices.push_back(getOrAddVertex(i1));
                                        part.indices.push_back(getOrAddVertex(i2));
                                    }
                                } else {
                                    // Trilist
                                    for (int32_t t = 0; t + 2 < strip.numIndices; t += 3) {
                                        uint16_t i0 = getSgIndex(strip.indexOffset + t);
                                        uint16_t i1 = getSgIndex(strip.indexOffset + t + 1);
                                        uint16_t i2 = getSgIndex(strip.indexOffset + t + 2);
                                        part.indices.push_back(getOrAddVertex(i0));
                                        part.indices.push_back(getOrAddVertex(i1));
                                        part.indices.push_back(getOrAddVertex(i2));
                                    }
                                }
                            }
                        } else {
                            // Direct trilist from strip group indices
                            for (int32_t t = 0; t + 2 < vtxSg->numIndices; t += 3) {
                                part.indices.push_back(getOrAddVertex(getSgIndex(t)));
                                part.indices.push_back(getOrAddVertex(getSgIndex(t + 1)));
                                part.indices.push_back(getOrAddVertex(getSgIndex(t + 2)));
                            }
                        }
                    }

                    if (part.IndexCount() > 0) {
                        modelData.parts.push_back(std::move(part));
                    }
                }
            }
        }
    }

    // 6. Physics Collision (.PHY)
    if (phyBytes.size() >= sizeof(PhyHeader)) {
        PhyHeader phyHdr {};
        std::memcpy(&phyHdr, phyBytes.data(), sizeof(PhyHeader));
        if (phyHdr.size == sizeof(PhyHeader) && phyHdr.solidCount > 0) {
            size_t curOfs = sizeof(PhyHeader);
            for (int32_t s = 0; s < phyHdr.solidCount; ++s) {
                if (curOfs + sizeof(PhyCompactSurface) > phyBytes.size()) {
                    break;
                }
                const auto* surf = reinterpret_cast<const PhyCompactSurface*>(phyBytes.data() + curOfs);
                if (std::memcmp(surf->vphysicsId, "VPHY", 4) == 0 && surf->size > 0) {
                    StudioPhysicsSolid  solid;
                    StudioPhysicsConvex hull;

                    // If compact surface has vertices, transform them
                    // Fallback to bounding box corners for this solid
                    hull.vertices.push_back(JPH::Vec3(modelData.hullMin.x, modelData.hullMin.y, modelData.hullMin.z));
                    hull.vertices.push_back(JPH::Vec3(modelData.hullMax.x, modelData.hullMax.y, modelData.hullMax.z));

                    solid.convexHulls.push_back(std::move(hull));
                    modelData.physicsSolids.push_back(std::move(solid));
                    curOfs += sizeof(int32_t) + surf->size;
                } else {
                    curOfs += sizeof(PhyCompactSurface);
                }
            }
        }
    }

    // Fallback: construct box hull if no physics solids were found
    if (modelData.physicsSolids.empty()) {
        StudioPhysicsSolid  solid;
        StudioPhysicsConvex hull;
        hull.vertices.push_back(JPH::Vec3(modelData.hullMin.x, modelData.hullMin.y, modelData.hullMin.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMax.x, modelData.hullMin.y, modelData.hullMin.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMax.x, modelData.hullMax.y, modelData.hullMin.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMin.x, modelData.hullMax.y, modelData.hullMin.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMin.x, modelData.hullMin.y, modelData.hullMax.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMax.x, modelData.hullMin.y, modelData.hullMax.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMax.x, modelData.hullMax.y, modelData.hullMax.z));
        hull.vertices.push_back(JPH::Vec3(modelData.hullMin.x, modelData.hullMax.y, modelData.hullMax.z));
        solid.convexHulls.push_back(std::move(hull));
        modelData.physicsSolids.push_back(std::move(solid));
    }

    return modelData;
}

} // namespace ZHLN::BSP

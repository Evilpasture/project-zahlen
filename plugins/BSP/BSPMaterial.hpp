// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BSPGeometry.hpp"
#include "StudioMaterial.hpp"
#include <Zahlen/Math/Geometry2D.hpp>
namespace ZHLN::BSP::BSPMaterial {
template <typename Upload>
auto Resolve(Upload&& upload, const SourceVFS& vfs, const MaterialStreams& part, const VMTMaterial* vmt) -> MaterialDesc {
    if (part.materialName.starts_with("tools/")) {
        return MaterialDesc::Unlit({0.4f, 0.4f, 0.4f, 1.0f});
    }

    // Neutral grey, not white: this desc is what ships when the VMT cannot be
    // found or parsed, and a pure-white fallback turns every missing material
    // into a blinding surface under direct light.
    MaterialDesc desc = MaterialDesc::Basic({0.5f, 0.5f, 0.5f, 1.0f}, 0.8f, 0.0f, false);

    if (vmt == nullptr) {
        return desc;
    }

    const VMTMaterial& material = *vmt;
    desc.baseColor              = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w};
    // $nocull from the VMT, or the importer's single-sheet wall audit marking
    // this part: Source draws undercovered faces from their back side (the
    // vbsp leaffaces of the room include them), so honours match by rendering
    // the whole material group double-sided. Backface physics is handled
    // separately via reversed triangles in the collision stream.
    desc.doubleSided = material.noCull || part.forceDoubleSided;
    if (material.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (material.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = material.alphaCutoff;
    }

    // Resolve base texture (albedo)
    if (!material.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(material.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = StudioMaterial::LoadTextureFromBytes(upload, material.baseTexture, *texBytes, true);
            }
        }
        if (desc.albedoMap == TextureID::Invalid) {
            // The VMT references art that is not on disk: darken the flat
            // baseColor so the surface reads as missing (dark grey) instead
            // of rendering as a pure-white wall.
            desc.baseColor.x *= 0.25f;
            desc.baseColor.y *= 0.25f;
            desc.baseColor.z *= 0.25f;
        }
    }

    // Resolve normal/bump map. The `true` at the end routes the pixels through
    // the DXT5nm swizzle: Source bumpmaps are not straight RGB normals.
    if (!material.bumpMap.empty()) {
        const auto bumpPath = vfs.ResolveTexture(material.bumpMap);
        if (bumpPath.has_value()) {
            const auto bumpBytes = vfs.ReadFile(*bumpPath);
            if (bumpBytes.has_value()) {
                desc.normalMap = StudioMaterial::LoadTextureFromBytes(upload, material.bumpMap, *bumpBytes, false, true);
            }
        }
    }

    // Source self-illumination is normally masked. Keep that spatial detail:
    // an explicit mask wins, otherwise the base texture is the mask Source's
    // $selfillum path uses. Sampling only a constant emissive factor made whole
    // materials glow uniformly and left the authored lamp pixels dark.
    if (const auto mask = material.Find("$selfillummask"); mask && !mask->empty()) {
        if (const auto maskPath = vfs.ResolveTexture(*mask); maskPath) {
            if (const auto maskBytes = vfs.ReadFile(*maskPath); maskBytes) {
                desc.emissiveMap = StudioMaterial::LoadTextureFromBytes(upload, *mask, *maskBytes, true);
            }
        }
    } else if (material.Find("$selfillum").has_value()) {
        desc.emissiveMap = desc.albedoMap;
    }

    // Shader class + emission, through the rules every material pass shares.
    ApplyVmtLighting(vfs, material, desc);

    return desc;
}

} // namespace ZHLN::BSP::BSPMaterial

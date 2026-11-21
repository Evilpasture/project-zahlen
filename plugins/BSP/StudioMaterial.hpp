// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "VMTLighting.hpp"
#include "VTFDecoder.hpp"
#include <Image/Image.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstring>
#include <limits>
namespace ZHLN::BSP::StudioMaterial {
template <typename Upload>
auto LoadTextureFromBytes(Upload& upload, std::string_view name, std::span<const std::byte> bytes, bool isSRGB = true, bool sourceNormal = false) -> TextureID {
    const auto image = Image::DecodeRGBA8({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    if (image) {
        const auto rgba = std::as_bytes(std::span(image->pixels));
        return upload(name, rgba, Extent2D {image->width, image->height}, isSRGB);
    }

    return TextureID::Invalid;
}

template <typename Upload>
auto Resolve(Upload&& upload, const SourceVFS& vfs, std::string_view materialName) -> MaterialDesc {
    MaterialDesc desc = MaterialDesc::Basic({1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.0f, false);
    const auto vmtPath = vfs.ResolveMaterial(materialName);
    if (!vmtPath.has_value()) {
        return desc;
    }

    const auto vmtBytes = vfs.ReadFile(*vmtPath);
    if (!vmtBytes.has_value() || vmtBytes->empty()) {
        return desc;
    }

    const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
    const auto             vmtMatExp = ResolveVmtMaterial(
        vmtText,
        [&vfs](std::string_view includePath) -> std::optional<std::string> {
            const auto includeBytes = vfs.ReadFile(std::string(includePath));
            if (!includeBytes.has_value() || includeBytes->empty()) {
                return std::nullopt;
            }
            return std::string(reinterpret_cast<const char*>(includeBytes->data()), includeBytes->size());
        }
    );
    if (!vmtMatExp.has_value()) {
        return desc;
    }

    const auto& vmt   = *vmtMatExp;
    desc.baseColor   = {vmt.baseColor.x, vmt.baseColor.y, vmt.baseColor.z, vmt.baseColor.w};
    desc.doubleSided = vmt.noCull;
    if (vmt.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (vmt.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = vmt.alphaCutoff;
    }

    if (!vmt.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = LoadTextureFromBytes(upload, vmt.baseTexture, *texBytes, true);
            }
        }
    }

    if (!vmt.bumpMap.empty()) {
        const auto bumpPath = vfs.ResolveTexture(vmt.bumpMap);
        if (bumpPath.has_value()) {
            const auto bumpBytes = vfs.ReadFile(*bumpPath);
            if (bumpBytes.has_value()) {
                desc.normalMap = LoadTextureFromBytes(upload, vmt.bumpMap, *bumpBytes, false, true);
            }
        }
    }

    // ---- shader class + emission -----------------------------------------
    // Source puts its lamps on props: a wall light, a ceiling panel, an
    // emergency light and a floodlight are all StudioModels whose material is
    // `VertexLitGeneric` + `$selfillum 1`, and a glowing screen or sign is
    // usually an `UnlitGeneric` panel. This pass reads the same keyvalues
    // through the same rules the world material pass applies
    // (ApplyVmtLighting), so a material renders identically on a prop and on a
    // brush face -- and so the lamp surfaces stop coming up as plain, dark,
    // lit albedo.
    ApplyVmtLighting(vfs, vmt, desc);

    return desc;
}

} // namespace ZHLN::BSP::StudioMaterial

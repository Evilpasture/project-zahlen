// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "VMTLighting.hpp"
#include "VTFDecoder.hpp"
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <cstring>
#include <limits>
#include <stb_image.h>
namespace ZHLN::BSP::StudioMaterial {
template <typename Upload>
auto LoadTextureFromBytes(Upload& upload, std::string_view name, std::span<const std::byte> bytes, bool isSRGB = true, bool sourceNormal = false) -> TextureID {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "VTF\0", 4) == 0) {
        auto vtfExp = DecodeVTF(bytes);
        if (vtfExp.has_value()) {
            if (sourceNormal) {
                ConvertSourceNormalInPlace(*vtfExp);
            }
            auto texRes = upload(name, vtfExp->rgba8, Extent2D {vtfExp->width, vtfExp->height}, isSRGB);
            if (texRes != TextureID::Invalid) {
                return texRes;
            }
        }
    }

    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return TextureID::Invalid;
    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4
    );
    if (pixels && width > 0 && height > 0) {
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(width) * height * 4);
        auto texRes = upload(name, rgba, Extent2D {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB);
        stbi_image_free(pixels);
        pixels = nullptr;
        if (texRes != TextureID::Invalid) {
            return texRes;
        }
    }

    if (pixels) stbi_image_free(pixels);
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

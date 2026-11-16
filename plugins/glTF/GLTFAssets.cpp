// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GLTFAssets.hpp"
#include <algorithm>
#include <cmath>
namespace ZHLN::GLTF {
namespace {
struct Pending {
    COM::IGraphicsDevice&  graphics;
    std::vector<Mesh>      meshes;
    std::vector<TextureID> textures;
    bool                   committed = false;
    ~Pending() {
        if (committed)
            return;
        for (const auto& mesh: meshes)
            (void) graphics.DestroyMesh(mesh);
        for (const auto texture: textures)
            graphics.UnloadTexture(texture);
    }
};
} // namespace
auto Upload(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics, const Document& document, UploadOptions options) -> std::expected<Asset, ErrorCode> {
    const auto& path = document.virtualPath;
    if (path.empty() || path.size() >= 256 || path.find('\0') != std::string::npos)
        return std::unexpected(COM::ComError::InvalidArgument);
    const auto hash = assets.PrefabHash(std::string_view(path));
    if (const auto cached = assets.DescribePrefab(hash); cached.HasValue()) {
        if (cached.value.emissiveFactorScale != document.options.emissiveFactorScale ||
            cached.value.maxTextureDimension != document.options.maxTextureDimension || !cached.value.gpuResourcesReady)
            return std::unexpected(COM::ComError::InvalidArgument);
        return Asset {.hash = hash};
    }
    for (const auto& part: document.parts)
        if (part.primitive >= document.primitives.size())
            return std::unexpected(COM::ComError::InvalidArgument);
    for (const auto& p: document.primitives) {
        if (p.positions.empty() || p.positions.size() > UINT32_MAX || p.indices.size() > UINT32_MAX || p.tangentFrames.size() != p.positions.size() ||
            p.surfaces.size() != p.positions.size() || (!p.skins.empty() && p.skins.size() != p.positions.size()) || p.activeMorphCount > 4 ||
            p.morphDeltas.size() != size_t(p.activeMorphCount) * p.positions.size() * 4)
            return std::unexpected(COM::ComError::InvalidArgument);
        for (const auto index: p.indices)
            if (index >= p.positions.size())
                return std::unexpected(COM::ComError::InvalidArgument);
        for (const auto& binding: p.textures)
            if (!binding.field || binding.imageIndex >= document.textures.size())
                return std::unexpected(COM::ComError::InvalidArgument);
    }
    Pending                pending {graphics};
    std::vector<TextureID> textures(document.textures.size(), TextureID::Invalid);
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto& image = document.textures[i];
        if (image.rgba.empty())
            continue; // original best-effort image policy
        if (!image.extent.width || !image.extent.height || uint64_t(image.extent.width) * image.extent.height * 4 != image.rgba.size())
            return std::unexpected(COM::ComError::InvalidArgument);
        const auto texture =
            graphics.CreateTexture(std::string_view(path + "#tex_" + std::to_string(i)), std::as_bytes(std::span(image.rgba)), image.extent, image.isSRGB);
        if (!texture.HasValue())
            return std::unexpected(texture.error);
        textures[i] = texture.value;
        if (std::find(pending.textures.begin(), pending.textures.end(), texture.value) == pending.textures.end())
            pending.textures.push_back(texture.value);
    }
    std::vector<Material>         materials;
    std::vector<uint32_t>         morphOffsets;
    std::vector<CollisionAssetID> collisions;
    for (size_t i = 0; i < document.primitives.size(); ++i) {
        const auto& p    = document.primitives[i];
        const auto  mesh = graphics.UploadMesh(
            COM::MeshStreamsDesc {.positions = p.positions, .tangentFrames = p.tangentFrames, .surfaces = p.surfaces, .skin = p.skins, .indices32 = p.indices},
            options.buildBLAS
        );
        if (!mesh.HasValue())
            return std::unexpected(mesh.error);
        pending.meshes.push_back(mesh.value);
        auto desc = p.material;
        for (const auto& binding: p.textures)
            desc.*binding.field = textures[binding.imageIndex];
        const auto material = graphics.CreateMaterial(desc);
        if (!material.HasValue())
            return std::unexpected(material.error);
        materials.push_back(material.value);
        uint32_t offset = 0;
        if (p.activeMorphCount) {
            const auto allocation = graphics.AllocateMorphDeltas(p.morphDeltas);
            if (!allocation.HasValue())
                return std::unexpected(allocation.error);
            offset = allocation.value;
        }
        morphOffsets.push_back(offset);
        CollisionAssetID collision = CollisionAssetID::Invalid;
        if (options.buildColliders && p.wantsMeshCollider && !p.indices.empty()) {
            collision = static_cast<CollisionAssetID>(assets.AssetHash(std::string_view(path + "#collision_" + std::to_string(i))));
            std::vector<FVec3> vertices;
            for (const auto& v: p.positions)
                vertices.push_back({v.position[0], v.position[1], v.position[2]});
            const auto cached = assets.CacheCollisionMesh(COM::InChain(COM::CollisionMeshInfo {.id = collision, .vertices = vertices, .indices = p.indices}));
            if (!cached.HasValue())
                return std::unexpected(cached.error);
        }
        collisions.push_back(collision);
    }
    std::vector<COM::PrefabPartInfo>        parts;
    std::vector<COM::PrefabCSGModifierInfo> csg;
    for (size_t i = 0; i < document.parts.size(); ++i) {
        const auto&         source    = document.parts[i];
        const auto&         primitive = document.primitives[source.primitive];
        const auto          key       = path + "#part" + std::to_string(i);
        COM::PrefabPartInfo part {
            .name                = source.name,
            .meshAsset           = assets.AssetHash(std::string_view(key)),
            .materialAsset       = assets.AssetHash(std::string_view(key + "_mat")),
            .mesh                = pending.meshes[source.primitive],
            .defaultMaterial     = materials[source.primitive],
            .isSkinned           = source.isSkinned,
            .nodeIndex           = source.nodeIndex,
            .skeletonIndex       = source.skeletonIndex,
            .morphOffset         = morphOffsets[source.primitive],
            .activeMorphCount    = primitive.activeMorphCount,
            .defaultMorphWeights = source.defaultMorphWeights,
            .boundingRadius      = primitive.boundingRadius,
            .localMin            = primitive.localMin,
            .localMax            = primitive.localMax,
            .collision           = collisions[source.primitive],
            .hasBoxCollider      = options.buildColliders
        };
        part.boxHalfExtents = {
            std::max(0.01f, (primitive.localMax.x - primitive.localMin.x) * 0.5f), std::max(0.01f, (primitive.localMax.y - primitive.localMin.y) * 0.5f),
            std::max(0.01f, (primitive.localMax.z - primitive.localMin.z) * 0.5f)
        };
        part.boxCenter    = (primitive.localMax + primitive.localMin) * 0.5f;
        part.csgModifiers = {static_cast<uint32_t>(csg.size()), static_cast<uint32_t>(source.csgModifiers.size())};
        for (const auto& modifier: source.csgModifiers)
            csg.push_back({modifier.operation, String64(modifier.operand_name.c_str())});
        parts.push_back(part);
    }
    std::vector<COM::PrefabSkeletonInfo> skeletons;
    std::vector<COM::PrefabJointInfo>    joints;
    for (const auto& source: document.skeletons) {
        skeletons.push_back({source.name, {static_cast<uint32_t>(joints.size()), static_cast<uint32_t>(source.joints.size())}});
        for (const auto& joint: source.joints)
            joints.push_back({joint.name, joint.parentIndex, joint.nodeIndex, joint.inverseBindMatrix});
    }
    std::vector<COM::PrefabAnimationInfo> animations;
    std::vector<COM::PrefabChannelInfo>   channels;
    std::vector<float>                    times, values;
    for (const auto& clip: document.animations) {
        animations.push_back({clip.name, clip.duration, {static_cast<uint32_t>(channels.size()), static_cast<uint32_t>(clip.channels.size())}});
        for (const auto& channel: clip.channels) {
            channels.push_back(
                {channel.targetNodeIndex,
                 channel.path,
                 channel.interpolation,
                 {static_cast<uint32_t>(times.size()), static_cast<uint32_t>(channel.keyTimes.size())},
                 {static_cast<uint32_t>(values.size()), static_cast<uint32_t>(channel.keyValues.size())}}
            );
            times.insert(times.end(), channel.keyTimes.begin(), channel.keyTimes.end());
            values.insert(values.end(), channel.keyValues.begin(), channel.keyValues.end());
        }
    }
    const auto registered = assets.RegisterPrefab(
        COM::InChain(
            COM::PrefabRegistrationInfo {
                .virtualPath         = std::string_view(path),
                .emissiveFactorScale = document.options.emissiveFactorScale,
                .maxTextureDimension = document.options.maxTextureDimension,
                .nodes               = document.nodes,
                .parts               = parts,
                .skeletons           = skeletons,
                .joints              = joints,
                .animations          = animations,
                .channels            = channels,
                .keyTimes            = times,
                .keyValues           = values,
                .lights              = document.lights,
                .csgModifiers        = csg
            }
        )
    );
    if (!registered.HasValue())
        return std::unexpected(registered.error);
    pending.committed = true;
    return Asset {.hash = registered.value, .textures = std::move(pending.textures)};
}
} // namespace ZHLN::GLTF

// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <engine/AssetManager.hpp>
#include <render/RenderContext.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>

namespace ZHLN {

namespace {

[[nodiscard]] constexpr auto CanonicalFloatBits(float v) noexcept -> uint32_t {
    return std::bit_cast<uint32_t>(v == 0.0f ? 0.0f : v);
}

} // namespace

auto HashProceduralMeshID(const ProceduralMeshRecipe& recipe) noexcept -> AssetID {
    std::size_t h = static_cast<std::size_t>(Hash64("ZHLN::ProceduralMesh::v1"));
    HashCombine(
        h,
        static_cast<std::size_t>(recipe.shape),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.dimensions.x)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.dimensions.y)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.dimensions.z)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.x)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.y)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.z)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.w)),
        static_cast<std::size_t>(recipe.buildBLAS ? 1u : 0u)
    );
    const auto id = static_cast<AssetID>(h);
    return id == InvalidAssetID ? AssetID {1} : id;
}

auto HashProceduralMaterialID(const ProceduralMaterialRecipe& recipe) noexcept -> MaterialID {
    std::size_t h = static_cast<std::size_t>(Hash64("ZHLN::ProceduralMaterial::v1"));
    HashCombine(
        h,
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.x)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.y)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.z)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.color.w)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.roughness)),
        static_cast<std::size_t>(CanonicalFloatBits(recipe.metallic))
    );
    const auto id = static_cast<MaterialID>(h);
    return id == InvalidMaterialID ? MaterialID {1} : id;
}

auto HashMaterialOverrideID(const Material& mat) noexcept -> MaterialID {
    std::size_t h = static_cast<std::size_t>(Hash64("ZHLN::MaterialOverride::v1"));
    HashCombine(
        h,
        static_cast<std::size_t>(mat.pipeline),
        static_cast<std::size_t>(mat.prePassPipeline),
        static_cast<std::size_t>(mat.albedoMap),
        static_cast<std::size_t>(mat.normalMap),
        static_cast<std::size_t>(mat.pbrMap),
        static_cast<std::size_t>(mat.emissiveMap),
        static_cast<std::size_t>(mat.bakedIrradianceMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.bakedIrradianceScale)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.baseColorFactor.x)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.baseColorFactor.y)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.baseColorFactor.z)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.baseColorFactor.w)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.emissiveFactor.x)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.emissiveFactor.y)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.emissiveFactor.z)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.emissiveFactor.w)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.metallicFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.roughnessFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.alphaCutoff)),
        static_cast<std::size_t>(mat.alphaMode),
        static_cast<std::size_t>(mat.doubleSided ? 1u : 0u),
        static_cast<std::size_t>(mat.unlit ? 1u : 0u),
        static_cast<std::size_t>(CanonicalFloatBits(mat.transmissionFactor)),
        static_cast<std::size_t>(mat.transmissionMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.iridescenceFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.filmThicknessNm)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.filmThicknessMinNm)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.volumeThicknessM)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.ior)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.normalScale)),
        static_cast<std::size_t>(mat.filmThicknessMap),
        static_cast<std::size_t>(mat.iridescenceMap),
        static_cast<std::size_t>(mat.volumeThicknessMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.clearcoatFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.clearcoatRoughnessFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.clearcoatNormalScale)),
        static_cast<std::size_t>(mat.clearcoatMap),
        static_cast<std::size_t>(mat.clearcoatRoughnessMap),
        static_cast<std::size_t>(mat.clearcoatNormalMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.anisotropyStrength)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.anisotropyRotation)),
        static_cast<std::size_t>(mat.anisotropyMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.sheenColorFactor.x)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.sheenColorFactor.y)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.sheenColorFactor.z)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.sheenRoughnessFactor)),
        static_cast<std::size_t>(mat.sheenColorMap),
        static_cast<std::size_t>(mat.sheenRoughnessMap),
        static_cast<std::size_t>(mat.occlusionMap),
        static_cast<std::size_t>(CanonicalFloatBits(mat.occlusionStrength)),
        static_cast<std::size_t>(mat.specularEnabled ? 1u : 0u),
        static_cast<std::size_t>(mat.environmentReflectionEnabled ? 1u : 0u),
        static_cast<std::size_t>(mat.environmentReflectionAlphaMask ? 1u : 0u),
        static_cast<std::size_t>(CanonicalFloatBits(mat.environmentReflectionFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.specularFactor)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.specularColorFactor.x)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.specularColorFactor.y)),
        static_cast<std::size_t>(CanonicalFloatBits(mat.specularColorFactor.z)),
        static_cast<std::size_t>(mat.specularMap),
        static_cast<std::size_t>(mat.specularTexMode)
    );
    for (const auto& sampler: mat.textureSamplers) {
        HashCombine(h, static_cast<std::size_t>(sampler.s), static_cast<std::size_t>(sampler.t));
    }
    for (const auto& xform: mat.textureTransforms) {
        HashCombine(
            h,
            static_cast<std::size_t>(CanonicalFloatBits(xform.offset.x)),
            static_cast<std::size_t>(CanonicalFloatBits(xform.offset.y)),
            static_cast<std::size_t>(CanonicalFloatBits(xform.scale.x)),
            static_cast<std::size_t>(CanonicalFloatBits(xform.scale.y)),
            static_cast<std::size_t>(CanonicalFloatBits(xform.rotation)),
            static_cast<std::size_t>(xform.texCoord)
        );
    }
    const auto id = static_cast<MaterialID>(h);
    return id == InvalidMaterialID ? MaterialID {1} : id;
}

AssetManager::~AssetManager() {
    ClearCache();
}

auto AssetManager::MountPak(std::string_view pakFilePath) -> std::expected<void, ErrorCode> {
    return _vfs.MountPak(pakFilePath);
}

auto AssetManager::MountDirectory(std::string_view directory, FS::MountOptions options) -> std::expected<void, ErrorCode> {
    return _vfs.MountDirectory(directory, options);
}

auto AssetManager::UnmountDirectory(std::string_view directory) -> std::expected<void, ErrorCode> {
    return _vfs.UnmountDirectory(directory);
}

void AssetManager::LoadAsync(RestrictSpan<FS::AsyncLoad> requests, TaskSystem::Counter* counter) const {
    _vfs.LoadAsync(requests, counter);
}

auto AssetManager::LoadSync(uint64_t assetID) const -> std::expected<FS::VirtualFile, ErrorCode> {
    return _vfs.LoadSync(assetID);
}

auto AssetManager::GetCachedPrefab(uint64_t hash) -> ZHLN::Optional<ModelPrefab&> {
    ModelPrefab* found = BorrowCachedPrefab(hash);
    return found != nullptr ? ZHLN::Optional<ModelPrefab&> {*found} : ZHLN::Optional<ModelPrefab&> {std::nullopt};
}

auto AssetManager::GetCachedPrefab(uint64_t hash) const noexcept -> ZHLN::Optional<const ModelPrefab&> {
    const ModelPrefab* found = BorrowCachedPrefab(hash);
    return found != nullptr ? ZHLN::Optional<const ModelPrefab&> {*found} : ZHLN::Optional<const ModelPrefab&> {std::nullopt};
}

auto AssetManager::BorrowCachedPrefab(uint64_t hash) const noexcept -> ModelPrefab* {
    ModelPrefab* found = nullptr;
    _prefabCache.With(hash, [&](ModelPrefab& prefab) { found = &prefab; });
    return found;
}

void AssetManager::CachePrefab(uint64_t hash, ModelPrefab* prefab) {
    _prefabCache.Insert(hash, prefab);
}

void AssetManager::CachePrefab(uint64_t hash, std::unique_ptr<ModelPrefab> prefab) {
    _prefabCache.Insert(hash, std::move(prefab));
}

void AssetManager::UseRenderContext(RenderContext& ctx) noexcept {
    if (_renderContext != nullptr && _renderContext != &ctx) {
        ReleaseCachedMeshBuffers();
    }
    _renderContext = &ctx;
}

void AssetManager::ReleaseCachedMeshBuffers() noexcept {
    if (_renderContext == nullptr) {
        return;
    }
    // First drop every alias, then retire buffers. Several ModelParts can
    // reference the same compiled glTF primitive; DestroyMesh is idempotent
    // for duplicate generational IDs.
    _prefabCache.ForEach([this](ModelPrefab& prefab) {
        for (const auto& part: prefab.parts) {
            if (part.meshAsset != InvalidAssetID) {
                _renderContext->UnregisterGPUMesh(part.meshAsset);
            }
        }
    });
    _prefabCache.ForEach([this](ModelPrefab& prefab) {
        for (auto& part: prefab.parts) {
            _renderContext->DestroyMesh(part.mesh);
            part.mesh = {};
        }
    });
    {
        ZHLN::MutexGuard guard(_proceduralMutex);
        for (auto& [id, entry]: _proceduralMeshes) {
            if (id != InvalidAssetID) {
                _renderContext->UnregisterGPUMesh(id);
            }
            if (entry.mesh.posBuffer != BufferID::Invalid) {
                _renderContext->DestroyMesh(entry.mesh);
                entry.mesh = {};
            }
        }
        for (auto& [id, entry]: _proceduralMaterials) {
            if (id != InvalidMaterialID) {
                _renderContext->UnregisterGPUMaterial(id);
            }
            entry.material.pipeline        = PipelineID::Invalid;
            entry.material.prePassPipeline = PipelineID::Invalid;
        }
    }
}

void AssetManager::InvalidateGPUMeshes() noexcept {
    // The old RenderContext's buffer pool is being destroyed. Do not pass its
    // IDs to the replacement renderer even if it reuses the same slots.
    _prefabCache.ForEach([](ModelPrefab& prefab) {
        prefab.gpuResourcesReady = false;
        for (auto& part: prefab.parts) {
            part.mesh                            = {};
            part.defaultMaterial.pipeline        = PipelineID::Invalid;
            part.defaultMaterial.prePassPipeline = PipelineID::Invalid;
        }
    });
    {
        ZHLN::MutexGuard guard(_proceduralMutex);
        for (auto& [id, entry]: _proceduralMeshes) {
            (void)id;
            entry.mesh = {};
        }
        for (auto& [id, entry]: _proceduralMaterials) {
            (void)id;
            entry.material.pipeline        = PipelineID::Invalid;
            entry.material.prePassPipeline = PipelineID::Invalid;
        }
    }
    _renderContext = nullptr;
}

auto AssetManager::FindProceduralMesh(AssetID id) const noexcept -> std::optional<Mesh> {
    if (id == InvalidAssetID) {
        return std::nullopt;
    }
    ZHLN::MutexGuard guard(_proceduralMutex);
    const auto       it = _proceduralMeshes.find(id);
    if (it == _proceduralMeshes.end() || it->second.mesh.posBuffer == BufferID::Invalid) {
        return std::nullopt;
    }
    return it->second.mesh;
}

void AssetManager::CacheProceduralMesh(AssetID id, const ProceduralMeshRecipe& recipe, const Mesh& mesh) {
    if (id == InvalidAssetID) {
        return;
    }
    Mesh replaced {};
    {
        ZHLN::MutexGuard guard(_proceduralMutex);
        auto&            entry = _proceduralMeshes[id];
        if (entry.mesh.posBuffer != BufferID::Invalid && entry.mesh.posBuffer != mesh.posBuffer) {
            replaced = entry.mesh;
        }
        entry.recipe = recipe;
        entry.mesh   = mesh;
    }
    if (replaced.posBuffer != BufferID::Invalid && _renderContext != nullptr) {
        _renderContext->DestroyMesh(replaced);
    }
}

auto AssetManager::FindProceduralMaterial(MaterialID id) const noexcept -> std::optional<Material> {
    if (id == InvalidMaterialID) {
        return std::nullopt;
    }
    ZHLN::MutexGuard guard(_proceduralMutex);
    const auto       it = _proceduralMaterials.find(id);
    if (it == _proceduralMaterials.end() || it->second.material.pipeline == PipelineID::Invalid) {
        return std::nullopt;
    }
    return it->second.material;
}

void AssetManager::CacheProceduralMaterial(MaterialID id, const ProceduralMaterialRecipe& recipe, const Material& material) {
    if (id == InvalidMaterialID) {
        return;
    }
    ZHLN::MutexGuard guard(_proceduralMutex);
    auto&            entry = _proceduralMaterials[id];
    entry.recipe           = recipe;
    entry.material         = material;
}

auto AssetManager::CopyProceduralMeshRecipes() const -> std::vector<std::pair<AssetID, ProceduralMeshRecipe>> {
    ZHLN::MutexGuard guard(_proceduralMutex);
    std::vector<std::pair<AssetID, ProceduralMeshRecipe>> out;
    out.reserve(_proceduralMeshes.size());
    for (const auto& [id, entry]: _proceduralMeshes) {
        out.emplace_back(id, entry.recipe);
    }
    return out;
}

auto AssetManager::CopyProceduralMaterialRecipes() const -> std::vector<std::pair<MaterialID, ProceduralMaterialRecipe>> {
    ZHLN::MutexGuard guard(_proceduralMutex);
    std::vector<std::pair<MaterialID, ProceduralMaterialRecipe>> out;
    out.reserve(_proceduralMaterials.size());
    for (const auto& [id, entry]: _proceduralMaterials) {
        out.emplace_back(id, entry.recipe);
    }
    return out;
}

auto AssetManager::CacheCollisionMesh(CollisionAssetID id, Physics::MeshGeometry geometry) -> bool {
    if (id == CollisionAssetID::Invalid || geometry.vertices.size() < 3 || geometry.indices.size() < 3 ||
        geometry.indices.size() % 3 != 0 || geometry.vertices.size() > UINT32_MAX || geometry.indices.size() > UINT32_MAX) return false;
    for (const auto& v: geometry.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return false;
    }
    bool anyTriangle = false;
    for (size_t i = 0; i < geometry.indices.size(); i += 3) {
        const auto a = geometry.indices[i], b = geometry.indices[i + 1], c = geometry.indices[i + 2];
        if (a >= geometry.vertices.size() || b >= geometry.vertices.size() || c >= geometry.vertices.size()) return false;
        anyTriangle |= a != b && b != c && a != c;
    }
    if (!anyTriangle) return false;
    auto snapshot = std::make_shared<const Physics::MeshGeometry>(std::move(geometry));
    ZHLN::MutexGuard guard(_collisionMutex);
    const auto rawID = static_cast<uint64_t>(id);
    if (!_collisionMeshes.With(rawID, [&](auto& current) { current = snapshot; })) {
        _collisionMeshes.Insert(rawID, std::make_unique<std::shared_ptr<const Physics::MeshGeometry>>(std::move(snapshot)));
    }
    return true;
}

auto AssetManager::FindCollisionMesh(CollisionAssetID id) const noexcept -> std::shared_ptr<const Physics::MeshGeometry> {
    if (id == CollisionAssetID::Invalid) return {};
    return _collisionMeshes.With(static_cast<uint64_t>(id), [](const auto& current) { return current; }).value_or(nullptr);
}

void AssetManager::ClearCache() noexcept {
    {
        ZHLN::MutexGuard guard(_collisionMutex);
        _collisionMeshes.Clear();
    }
    ReleaseCachedMeshBuffers();
    {
        ZHLN::MutexGuard guard(_proceduralMutex);
        _proceduralMeshes.clear();
        _proceduralMaterials.clear();
    }
    _prefabCache.Clear();
    _environmentImages.Clear();
}

uint32_t AssetManager::GetCachedPrefabs(ModelPrefab** outPrefabs, uint32_t maxCount) {
    return _prefabCache.GetAll(outPrefabs, maxCount);
}

auto AssetManager::CacheEnvironmentImage(std::string_view key, EnvironmentImage image) -> bool {
    if (key.empty() || image.width == 0 || image.height == 0 || static_cast<size_t>(image.width) > std::numeric_limits<size_t>::max() / 4u / image.height ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4u ||
        (!image.lightingRgba.empty() && image.lightingRgba.size() != image.rgba.size()) || (image.sun.has_value() && image.lightingRgba.empty())) {
        return false;
    }
    if (image.sun) {
        float directionLength = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float dir      = image.sun->direction[c];
            const float strength = image.sun->irradiance[c];
            if (!std::isfinite(dir) || !std::isfinite(strength) || strength < 0.0f) {
                return false;
            }
            directionLength += dir * dir;
        }
        if (!std::isfinite(directionLength) || std::abs(directionLength - 1.0f) > 0.01f) {
            return false;
        }
        for (const auto& coeff: image.sun->diffuseSH) {
            for (float channel: coeff) {
                if (!std::isfinite(channel)) {
                    return false;
                }
            }
        }
    }
    _environmentImages.Insert(HashAssetPath(key), std::make_unique<EnvironmentImage>(std::move(image)));
    return true;
}

auto AssetManager::FindEnvironmentImage(std::string_view key) const noexcept -> std::optional<EnvironmentImageView> {
    if (key.empty()) {
        return std::nullopt;
    }
    // The lock is held while the view is built, so the spans it carries stay
    // valid until the caller's next ClearCache (the documented contract).
    // With() returns the value snapshot directly; no out-parameter needed.
    return _environmentImages.With(HashAssetPath(key), [](const EnvironmentImage& image) {
        return EnvironmentImageView {
            .rgba         = image.rgba,
            .lightingRgba = image.lightingRgba,
            .sun          = image.sun,
            .width        = image.width,
            .height       = image.height,
            .contentHash  = image.contentHash,
        };
    });
}

} // namespace ZHLN

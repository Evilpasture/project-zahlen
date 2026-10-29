// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Math3D.hpp"
#include <Zahlen/Render/Render.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace ZHLN {

namespace {

[[nodiscard]] constexpr auto EncodeMaterialSamplerWord(const MaterialSamplerAddresses& addresses, size_t first) noexcept -> uint32_t {
    uint32_t packed = 0;
    for (size_t i = 0; i < 8 && first + i < addresses.size(); ++i) {
        const auto     s    = static_cast<uint32_t>(addresses[first + i].s);
        const auto     t    = static_cast<uint32_t>(addresses[first + i].t);
        const uint32_t code = (s < 3 && t < 3) ? s * 3 + t : 0;
        packed |= code << (i * 4);
    }
    return packed;
}

static_assert(kMaterialTextureSlotCount <= 16);

constexpr MaterialSamplerAddresses kAllAddressModes = [] {
    MaterialSamplerAddresses modes {};
    for (uint32_t i = 0; i < kMaterialSamplerVariantCount; ++i) {
        modes[i] = {static_cast<TextureWrap>(i / 3), static_cast<TextureWrap>(i % 3)};
    }
    return modes;
}();
static_assert(EncodeMaterialSamplerWord(kAllAddressModes, 0) == 0x76543210u);
static_assert(EncodeMaterialSamplerWord(kAllAddressModes, 8) == 0x8u);
static_assert(EncodeMaterialSamplerWord(MaterialSamplerAddresses {}, 0) == 0u);
static_assert([] {
    MaterialSamplerAddresses modes {};
    modes[static_cast<size_t>(MaterialTextureSlot::Transmission)] = {TextureWrap::MirroredRepeat, TextureWrap::ClampToEdge};
    return EncodeMaterialSamplerWord(modes, 8) == (7u << 24);
}());

struct ResolvedMeshMaterial {
    NativeMesh*     posMesh         = nullptr;
    NativeMesh*     frameMesh       = nullptr;
    NativeMesh*     surfaceMesh     = nullptr;
    NativeMesh*     finalPosMesh    = nullptr;
    NativeMesh*     skinMesh        = nullptr;
    NativeMesh*     indexMesh       = nullptr;
    NativeMaterial* material        = nullptr;
    NativeMaterial* prePassMaterial = nullptr;

    Vk::DeviceAddress<VertexPosition>     posAddr;
    Vk::DeviceAddress<VertexTangentFrame> frameAddr;
    Vk::DeviceAddress<VertexSurface>      surfaceAddr;

    Vk::DeviceAddress<GPUMeshlet> meshletAddr;
    Vk::DeviceAddress<uint32_t>   meshletVertexAddr;
    Vk::DeviceAddress<uint8_t>    meshletTriAddr;
    uint32_t                      meshletCount = 0;
};

[[nodiscard]] inline bool MeshletsUsable(const Mesh& mesh, BufferHandle skinnedVertexBuffer) noexcept {
    return mesh.meshletCount > 0 && mesh.meshletBuffer != BufferHandle::Invalid && mesh.meshletVertexBuffer != BufferHandle::Invalid &&
           mesh.meshletTriBuffer != BufferHandle::Invalid && skinnedVertexBuffer == BufferHandle::Invalid;
}

struct BindlessIndices {
    uint32_t albedo;
    uint32_t normal;
    uint32_t pbr;
    uint32_t emissive;
};

constexpr uint32_t kNoFilmTexture = 0xFFFFu;

[[nodiscard]] uint32_t OptionalTextureIndex(RenderContext::Impl* impl, TextureHandle handle) noexcept {
    if (handle == TextureHandle::Invalid) {
        return kNoFilmTexture;
    }
    return impl->textureManager.GetBindlessIndex(handle) & kNoFilmTexture;
}

[[nodiscard]] BindlessIndices ResolveMaterialTextures(RenderContext::Impl* impl, const Material& material) noexcept {
    return {
        .albedo   = (material.albedoMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.albedoMap) : 1,
        .normal   = (material.normalMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.normalMap) : 2,
        .pbr      = (material.pbrMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.pbrMap) : 1,
        .emissive = (material.emissiveMap != TextureHandle::Invalid) ? impl->textureManager.GetBindlessIndex(material.emissiveMap) : 1
    };
}

struct InstanceDataDesc {
    const ResolvedMeshMaterial* resolved = nullptr;

    JPH::Mat44 world     = JPH::Mat44::sIdentity();
    JPH::Mat44 prevWorld = JPH::Mat44::sIdentity();

    uint64_t posAddress          = 0;
    uint64_t tangentFrameAddress = 0;
    uint64_t surfaceAddress      = 0;

    BindlessIndices indices {};
    uint32_t bakedIrradianceTex = kNoFilmTexture;
    float    bakedIrradianceScale = 1.0f;
    bool                       hasAmbientCube = false;
    std::array<JPH::Float4, 6> ambientCube {};

    uint32_t alphaMode    = 0;
    bool     isViewmodel  = false;
    bool     isBackground = false;
    bool     isSkinned    = false;
    bool     doubleSided  = false;
    bool     unlit        = false;
    bool     hasEmissive  = false;

    uint32_t vertexCount      = 0;
    uint32_t indexCount       = 0;
    uint32_t jointOffset      = 0;
    uint32_t morphOffset      = 0;
    uint32_t activeMorphCount = 0;

    float cullRadius      = 0.0f;
    float metallicFactor  = 0.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff     = 0.0f;

    JPH::Float3 localCenter {0.0f, 0.0f, 0.0f};
    JPH::Float4 morphWeights {0.0f, 0.0f, 0.0f, 0.0f};
    JPH::Float4 baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4 emissiveFactor {0.0f, 0.0f, 0.0f, 1.0f};

    float                     transmissionFactor       = 0.0f;
    float                     iridescenceFactor        = 0.0f;
    float                     filmThicknessNm          = 0.0f;
    float                     filmThicknessMinNm       = 0.0f;
    float                     volumeThicknessM         = 0.0f;
    float                     ior                      = 1.5f;
    float                     normalScale              = 1.0f;
    uint32_t                  transmissionTex          = kNoFilmTexture;
    uint32_t                  filmThicknessTex         = kNoFilmTexture;
    uint32_t                  iridescenceTex           = kNoFilmTexture;
    uint32_t                  volumeThicknessTex       = kNoFilmTexture;
    float                     clearcoatFactor          = 0.0f;
    float                     clearcoatRoughnessFactor = 0.0f;
    float                     clearcoatNormalScale     = 1.0f;
    uint32_t                  clearcoatTex             = kNoFilmTexture;
    uint32_t                  clearcoatRoughnessTex    = kNoFilmTexture;
    uint32_t                  clearcoatNormalTex       = kNoFilmTexture;
    float                     anisotropyStrength       = 0.0f;
    float                     anisotropyRotation       = 0.0f;
    uint32_t                  anisotropyTex            = kNoFilmTexture;
    JPH::Float3               sheenColorFactor {};
    float                     sheenRoughnessFactor = 0.0f;
    uint32_t                  sheenColorTex        = kNoFilmTexture;
    uint32_t                  sheenRoughnessTex    = kNoFilmTexture;
    uint32_t                  occlusionTex         = kNoFilmTexture;
    float                     occlusionStrength    = 1.0f;
    bool                      specularEnabled      = false;
    bool                      environmentReflectionEnabled  = false;
    bool                      environmentReflectionAlphaMask = false;
    float                     environmentReflectionFactor = 1.0f;
    float                     specularFactor       = 1.0f;
    JPH::Float3               specularColorFactor {1.0f, 1.0f, 1.0f};
    uint32_t                  specularTex     = kNoFilmTexture;
    uint32_t                  specularTexMode = 0;
    MaterialSamplerAddresses  textureSamplers {};
    MaterialTextureTransforms textureTransforms {};
};

[[nodiscard]] inline auto BuildGPUInstanceData(const InstanceDataDesc& desc) noexcept -> InstanceData {
    const ResolvedMeshMaterial* res = desc.resolved;

    const uint32_t isViewmodel     = desc.isViewmodel ? 1u : 0u;
    const uint32_t isBackground    = desc.isBackground ? 1u : 0u;
    const uint32_t isSkinned       = desc.isSkinned ? 1u : 0u;
    const uint32_t doubleSided     = desc.doubleSided ? 1u : 0u;
    const uint32_t hasTransmission = !desc.unlit && desc.transmissionFactor > 0.0f ? 1u : 0u;
    const uint32_t isUnlit         = desc.unlit ? 1u : 0u;
    // Bits 14..18 are deliberately material-presence flags, rather than a
    // sampled result: a texture can make the extension non-zero per pixel.
    constexpr float kExtensionEpsilon = 1.0f / 255.0f;
    const uint32_t hasClearcoat =
        (!desc.unlit &&
         (desc.clearcoatFactor > kExtensionEpsilon || desc.clearcoatTex != kNoFilmTexture || desc.clearcoatRoughnessTex != kNoFilmTexture ||
          desc.clearcoatNormalTex != kNoFilmTexture)) ?
            1u :
            0u;
    const uint32_t hasAnisotropy = (!desc.unlit && (desc.anisotropyStrength > kExtensionEpsilon || desc.anisotropyTex != kNoFilmTexture)) ? 1u : 0u;
    const uint32_t hasSheen =
        (!desc.unlit &&
         (desc.sheenColorFactor.x > kExtensionEpsilon || desc.sheenColorFactor.y > kExtensionEpsilon || desc.sheenColorFactor.z > kExtensionEpsilon ||
          desc.sheenColorTex != kNoFilmTexture || desc.sheenRoughnessTex != kNoFilmTexture)) ?
            1u :
            0u;
    const uint32_t hasEmissive = desc.hasEmissive ? 1u : 0u;
    // Bit 20: per-instance Source ambient cube present (InstanceData::ambientCube).
    const uint32_t hasAmbientCube = desc.hasAmbientCube ? 1u : 0u;
    const uint32_t hasSpecular = !desc.unlit && desc.specularEnabled ? 1u : 0u;

    const uint32_t isMirrored = desc.world.GetDeterminant3x3() < 0.0f ? 1u : 0u;

    JPH::Float4 emissive       = desc.emissiveFactor;
    uint32_t    paddingCenter  = 0;
    uint32_t    paddingMeshlet = 0;
    if (hasTransmission != 0) {
        paddingCenter  = (desc.volumeThicknessTex << 16) | (desc.filmThicknessTex & kNoFilmTexture);
        paddingMeshlet = desc.iridescenceTex & kNoFilmTexture;
    } else {
        const uint32_t coat8 = static_cast<uint32_t>(std::clamp(desc.clearcoatFactor, 0.0f, 1.0f) * 255.0f + 0.5f);
        if (coat8 != 0) {
            const uint32_t scale8 = static_cast<uint32_t>(std::clamp(desc.clearcoatNormalScale * 64.0f, 0.0f, 255.0f) + 0.5f);
            emissive.w            = desc.clearcoatRoughnessFactor;
            paddingCenter         = (desc.clearcoatRoughnessTex << 16) | (desc.clearcoatTex & kNoFilmTexture);
            paddingMeshlet        = (scale8 << 24) | (coat8 << 16) | (desc.clearcoatNormalTex & kNoFilmTexture);
        }
    }

    std::array<JPH::Float4, kMaterialTextureSlotCount> uvRow0;
    std::array<JPH::Float4, kMaterialTextureSlotCount> uvRow1;
    for (size_t slot = 0; slot < uvRow0.size(); ++slot) {
        const auto& transform = desc.textureTransforms[slot];
        const float c         = transform.rotation == 0.0f ? 1.0f : std::cos(transform.rotation);
        const float s         = transform.rotation == 0.0f ? 0.0f : std::sin(transform.rotation);
        uvRow0[slot]          = JPH::Float4 {c * transform.scale.x, s * transform.scale.y, transform.offset.x, 0.0f};
        uvRow1[slot]          = JPH::Float4 {-s * transform.scale.x, c * transform.scale.y, transform.offset.y, static_cast<float>(transform.texCoord)};
    }

    return InstanceData {
        .world               = desc.world,
        .prevWorld           = desc.prevWorld,
        .posAddress          = desc.posAddress,
        .tangentFrameAddress = desc.tangentFrameAddress,
        .surfaceAddress      = desc.surfaceAddress,
        .skinAddress         = (res != nullptr && res->skinMesh != nullptr) ? res->skinMesh->vboAddress.Get() : 0ull,
        .iboAddress          = (res != nullptr && res->indexMesh != nullptr) ? res->indexMesh->vboAddress.Get() : 0ull,
        .vertexCount         = desc.vertexCount,
        .indexCount          = desc.indexCount,
        .texIndices0         = (desc.indices.normal << 16) | (desc.indices.albedo & 0xFFFFu),
        .texIndices1         = (desc.indices.emissive << 16) | (desc.indices.pbr & 0xFFFFu),
        .cullRadius          = desc.cullRadius,
        .metallicFactor      = desc.metallicFactor,
        .roughnessFactor     = desc.roughnessFactor,
        .alphaCutoff         = desc.alphaCutoff,

        .flags            = (hasAmbientCube << 20) | (isViewmodel << 19) | (hasSpecular << 18) | (hasEmissive << 17) | (hasSheen << 16) | (hasAnisotropy << 15) |
                            (hasClearcoat << 14) | (isBackground << 13) | (isMirrored << 12) | (isUnlit << 11) | (hasTransmission << 10) |
                            (doubleSided << 9) | (isSkinned << 8) | (desc.alphaMode & 0xFFu),
        .jointOffset      = desc.jointOffset,
        .morphOffset      = desc.morphOffset,
        .activeMorphCount = desc.activeMorphCount,
        .localCenter      = desc.localCenter,
        ._paddingCenter   = paddingCenter,
        .morphWeights     = desc.morphWeights,
        .baseColorFactor  = desc.baseColorFactor,
        .emissiveFactor   = emissive,
        .meshletAddress   = (res != nullptr) ? res->meshletAddr.Get() : 0ull,
        .meshletVertexAddress = (res != nullptr) ? res->meshletVertexAddr.Get() : 0ull,
        .meshletTriAddress    = (res != nullptr) ? res->meshletTriAddr.Get() : 0ull,
        .meshletCount         = (res != nullptr) ? res->meshletCount : 0u,
        ._paddingMeshlet      = paddingMeshlet,
        .anisotropyStrength   = std::clamp(desc.anisotropyStrength, 0.0f, 1.0f),
        .anisotropyRotation   = desc.anisotropyRotation,
        .anisotropyTexIndex   = desc.anisotropyTex,
        .samplerCodes0        = PackMaterialSamplerAddresses(desc.textureSamplers, 0),
        .samplerCodes1        = PackMaterialSamplerAddresses(desc.textureSamplers, 8),
        .sheenParams          = {desc.sheenColorFactor.x, desc.sheenColorFactor.y, desc.sheenColorFactor.z, std::clamp(desc.sheenRoughnessFactor, 0.0f, 1.0f)},
        .sheenColorTexIndex   = desc.sheenColorTex,
        .sheenRoughnessTexIndex  = desc.sheenRoughnessTex,
        .occlusionTexIndex       = desc.occlusionTex,
        .occlusionStrength       = std::clamp(desc.occlusionStrength, 0.0f, 1.0f),
        .transmissionValue       = desc.transmissionFactor,
        .transmissionIor         = desc.ior,
        .transmissionThicknessM  = desc.volumeThicknessM,
        .transmissionNormalScale = desc.normalScale,
        .iridescenceFactor       = desc.iridescenceFactor,
        .filmThicknessNm         = desc.filmThicknessNm,
        .filmThicknessMinNm      = desc.filmThicknessMinNm,
        .transmissionTexIndex    = desc.transmissionTex,
        .uvRow0                  = uvRow0,
        .uvRow1                  = uvRow1,
        .specularParams          = {desc.specularColorFactor.x, desc.specularColorFactor.y, desc.specularColorFactor.z, std::max(desc.specularFactor, 0.0f)},
        .specularTexIndex        = desc.specularTex,
        .specularFlags           = (desc.specularEnabled ? 1u : 0u) | (std::min(desc.specularTexMode, 3u) << 1u) |
                                  (desc.environmentReflectionEnabled ? 1u << 3u : 0u) | (desc.environmentReflectionAlphaMask ? 1u << 4u : 0u),
        .bakedIrradianceTexIndex = desc.bakedIrradianceTex,
        .bakedIrradianceScale    = std::max(desc.bakedIrradianceScale, 0.0f),
        .ambientCube             = desc.ambientCube,
        .environmentReflectionFactor = desc.environmentReflectionFactor,
    };
}

[[nodiscard]] std::optional<ResolvedMeshMaterial>
    ResolveDrawInputs(RenderContext::Impl* impl, const Material& material, const Mesh& mesh, BufferHandle skinnedVertexBuffer) noexcept {
    using enum BufferHandle;

    auto* posMesh        = impl->geometry.Resolve(mesh.posBuffer);
    auto* frameMesh      = impl->geometry.Resolve(mesh.tangentFrameBuffer);
    auto* surfaceMesh    = impl->geometry.Resolve(mesh.surfaceBuffer);
    auto* nativeMaterial = impl->pipelines.Resolve(material.pipeline);

    if (posMesh == nullptr || surfaceMesh == nullptr || (mesh.tangentFrameBuffer != Invalid && frameMesh == nullptr) ||
        (skinnedVertexBuffer != Invalid && frameMesh == nullptr) || nativeMaterial == nullptr) [[unlikely]] {
        return std::nullopt;
    }

    ResolvedMeshMaterial res;
    res.posMesh     = posMesh;
    res.frameMesh   = frameMesh;
    res.surfaceMesh = surfaceMesh;
    res.material    = nativeMaterial;

    if (material.prePassPipeline != PipelineHandle::Invalid) {
        res.prePassMaterial = impl->pipelines.Resolve(material.prePassPipeline);
    }

    res.skinMesh  = (mesh.skinBuffer != Invalid) ? impl->geometry.Resolve(mesh.skinBuffer) : nullptr;
    res.indexMesh = (mesh.indexBuffer != Invalid) ? impl->geometry.Resolve(mesh.indexBuffer) : nullptr;

    res.finalPosMesh = (skinnedVertexBuffer != Invalid) ? impl->geometry.Resolve(skinnedVertexBuffer) : res.posMesh;

    if (res.finalPosMesh == nullptr) [[unlikely]] {
        return std::nullopt;
    }
    if (skinnedVertexBuffer != Invalid && (res.finalPosMesh->vertexCount < posMesh->vertexCount ||
                                           res.finalPosMesh->buffer.Size() < static_cast<size_t>(res.finalPosMesh->vertexCount) *
                                                                                 (sizeof(VertexPosition) + sizeof(VertexTangentFrame)))) [[unlikely]] {
        return std::nullopt;
    }

    res.posAddr     = Vk::DeviceAddress<VertexPosition>::Cast(res.finalPosMesh->vboAddress);
    res.frameAddr   = res.frameMesh != nullptr ? Vk::DeviceAddress<VertexTangentFrame>::Cast(res.frameMesh->vboAddress) :
                                                 Vk::DeviceAddress<VertexTangentFrame> {nullptr};
    res.surfaceAddr = Vk::DeviceAddress<VertexSurface>::Cast(res.surfaceMesh->vboAddress);

    if (MeshletsUsable(mesh, skinnedVertexBuffer)) {
        auto* meshletMesh = impl->geometry.Resolve(mesh.meshletBuffer);
        auto* meshletVtx  = impl->geometry.Resolve(mesh.meshletVertexBuffer);
        auto* meshletTri  = impl->geometry.Resolve(mesh.meshletTriBuffer);

        if (meshletMesh != nullptr && meshletVtx != nullptr && meshletTri != nullptr) {
            res.meshletAddr       = Vk::DeviceAddress<GPUMeshlet>::Cast(meshletMesh->vboAddress);
            res.meshletVertexAddr = Vk::DeviceAddress<uint32_t>::Cast(meshletVtx->vboAddress);
            res.meshletTriAddr    = Vk::DeviceAddress<uint8_t>::Cast(meshletTri->vboAddress);
            res.meshletCount      = mesh.meshletCount;
        }
    }

    if (res.posMesh == res.surfaceMesh) {
        res.surfaceAddr = Vk::DeviceAddress<VertexSurface>::Cast(res.posAddr + static_cast<std::ptrdiff_t>(RenderContext::Impl::kMaxDebugVertices));
    }
    if (skinnedVertexBuffer != Invalid) {
        res.frameAddr = Vk::DeviceAddress<VertexTangentFrame>::Cast(res.posAddr + static_cast<std::ptrdiff_t>(res.finalPosMesh->vertexCount));
    }

    return res;
}

} // namespace

uint32_t PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept {
    return EncodeMaterialSamplerWord(addresses, first);
}

struct DrawAddressPack {
    Vk::DeviceAddress<>                   blasAddress;
    Vk::DeviceAddress<VertexPosition>     skinPosAddress;
    Vk::DeviceAddress<VertexTangentFrame> skinFrameAddress;
    Vk::DeviceAddress<uint32_t>           skinDataAddress;
};

[[nodiscard]] DrawAddressPack AddressesOf(const ResolvedMeshMaterial& res) noexcept {
    return {
        .blasAddress      = res.posMesh->blasAddress,
        .skinPosAddress   = Vk::DeviceAddress<VertexPosition>::Cast(res.posMesh->vboAddress),
        .skinFrameAddress = res.frameMesh != nullptr ? Vk::DeviceAddress<VertexTangentFrame>::Cast(res.frameMesh->vboAddress) :
                                                       Vk::DeviceAddress<VertexTangentFrame> {nullptr},
        .skinDataAddress  = res.skinMesh != nullptr ? Vk::DeviceAddress<uint32_t>::Cast(res.skinMesh->vboAddress) : Vk::DeviceAddress<uint32_t> {nullptr},
    };
}

void RenderContext::Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept {
    using enum DrawFlags;
    using enum BufferHandle;

    auto resolved = ResolveDrawInputs(_impl.get(), material, mesh, params.skinnedVertexBuffer);
    if (!resolved) [[unlikely]] {
        if (_impl->diagnostics.invalidDrawWarningCount.fetch_add(1, std::memory_order::relaxed) < 5) {
            ZHLN::LogWarning("RenderContext::Draw skipped draw call with invalid mesh, material, or skinned scratch handle.");
        }
        return;
    }

    if (params.skinnedVertexBuffer != Invalid) {
        _impl->frameState.hasSkinned = true;
    }

    auto tex = ResolveMaterialTextures(_impl.get(), material);

    uint32_t isViewmodel      = ((params.flags & DrawFlags::Viewmodel) != DrawFlags::None) ? 1u : 0u;
    uint32_t isSkinned        = (params.skinnedVertexBuffer == Invalid && (params.flags & Skinned) != None) ? 1u : 0u;
    uint32_t activeMorphCount = (params.skinnedVertexBuffer != Invalid) ? 0 : params.activeMorphCount;

    const auto addr = AddressesOf(*resolved);
    _impl->queues.Draws().push_back(
        {.instanceData = BuildGPUInstanceData(
             InstanceDataDesc {
                 .resolved                 = &*resolved,
                 .world                    = params.transform,
                 .prevWorld                = params.prevTransform,
                 .posAddress               = resolved->posAddr.Get(),
                 .tangentFrameAddress      = resolved->frameAddr.Get(),
                 .surfaceAddress           = resolved->surfaceAddr.Get(),
                 .indices                  = tex,
                 .bakedIrradianceTex     = OptionalTextureIndex(_impl.get(), material.bakedIrradianceMap),
                 .bakedIrradianceScale   = material.bakedIrradianceScale,
                 .hasAmbientCube         = params.hasAmbientCube,
                 .ambientCube            = params.ambientCube,
                 .alphaMode                = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                 .isViewmodel              = isViewmodel != 0u,
                 .isBackground             = (params.flags & DrawFlags::Background) != DrawFlags::None,
                 .isSkinned                = isSkinned != 0u,
                 .doubleSided              = material.doubleSided,
                 .unlit                    = material.unlit,
                 .hasEmissive              = material.bakedIrradianceMap != TextureHandle::Invalid || material.emissiveMap != TextureHandle::Invalid ||
                                             material.emissiveFactor.x > 0.0f || material.emissiveFactor.y > 0.0f || material.emissiveFactor.z > 0.0f ||
                                             params.hasAmbientCube,
                 .vertexCount              = resolved->posMesh->vertexCount,
                 .indexCount               = mesh.indexCount,
                 .jointOffset              = params.jointOffset,
                 .morphOffset              = params.morphOffset,
                 .activeMorphCount         = activeMorphCount,
                 .cullRadius               = params.cullRadius,
                 .metallicFactor           = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
                 .roughnessFactor          = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
                 .alphaCutoff              = material.alphaCutoff,
                 .localCenter              = params.localCenter,
                 .morphWeights             = params.morphWeights,
                 .baseColorFactor          = (params.colorOverride.w >= 0.0f) ? params.colorOverride : material.baseColorFactor,
                 .emissiveFactor           = (params.emissiveOverride.w >= 0.0f) ? params.emissiveOverride : material.emissiveFactor,
                 .transmissionFactor       = material.transmissionFactor,
                 .iridescenceFactor        = material.iridescenceFactor,
                 .filmThicknessNm          = material.filmThicknessNm,
                 .filmThicknessMinNm       = material.filmThicknessMinNm,
                 .volumeThicknessM         = material.volumeThicknessM,
                 .ior                      = material.ior,
                 .normalScale              = material.normalScale,
                 .transmissionTex          = OptionalTextureIndex(_impl.get(), material.transmissionMap),
                 .filmThicknessTex         = OptionalTextureIndex(_impl.get(), material.filmThicknessMap),
                 .iridescenceTex           = OptionalTextureIndex(_impl.get(), material.iridescenceMap),
                 .volumeThicknessTex       = OptionalTextureIndex(_impl.get(), material.volumeThicknessMap),
                 .clearcoatFactor          = material.clearcoatFactor,
                 .clearcoatRoughnessFactor = material.clearcoatRoughnessFactor,
                 .clearcoatNormalScale     = material.clearcoatNormalScale,
                 .clearcoatTex             = OptionalTextureIndex(_impl.get(), material.clearcoatMap),
                 .clearcoatRoughnessTex    = OptionalTextureIndex(_impl.get(), material.clearcoatRoughnessMap),
                 .clearcoatNormalTex       = OptionalTextureIndex(_impl.get(), material.clearcoatNormalMap),
                 .anisotropyStrength       = material.anisotropyStrength,
                 .anisotropyRotation       = material.anisotropyRotation,
                 .anisotropyTex            = OptionalTextureIndex(_impl.get(), material.anisotropyMap),
                 .sheenColorFactor         = material.sheenColorFactor,
                 .sheenRoughnessFactor     = material.sheenRoughnessFactor,
                 .sheenColorTex            = OptionalTextureIndex(_impl.get(), material.sheenColorMap),
                 .sheenRoughnessTex        = OptionalTextureIndex(_impl.get(), material.sheenRoughnessMap),
                 .occlusionTex             = OptionalTextureIndex(_impl.get(), material.occlusionMap),
                 .occlusionStrength        = material.occlusionStrength,
                 .specularEnabled          = material.specularEnabled,
                 .environmentReflectionEnabled  = material.environmentReflectionEnabled,
                 .environmentReflectionAlphaMask = material.environmentReflectionAlphaMask,
                 .environmentReflectionFactor = material.environmentReflectionFactor,
                 .specularFactor           = material.specularFactor,
                 .specularColorFactor      = material.specularColorFactor,
                 .specularTex              = OptionalTextureIndex(_impl.get(), material.specularMap),
                 .specularTexMode          = material.specularTexMode,
                 .textureSamplers          = material.textureSamplers,
                 .textureTransforms        = material.textureTransforms,
             }
         ),
         .material            = resolved->material,
         .prePassMaterial     = resolved->prePassMaterial,
         .blasAddress         = addr.blasAddress,
         .skinPosAddress      = addr.skinPosAddress,
         .skinFrameAddress    = addr.skinFrameAddress,
         .skinDataAddress     = addr.skinDataAddress,
         .skinnedVertexBuffer = params.skinnedVertexBuffer,
         .jointOffset         = params.jointOffset,
         .morphOffset         = params.morphOffset,
         .activeMorphCount    = params.activeMorphCount,
         .morphWeights        = params.morphWeights,
         .flags               = params.flags}
    );
}

void RenderContext::DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept {
    auto MakeCommand = [&](const Material& material, const Mesh& mesh, const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius,
                           uint32_t jointOffset, BufferHandle skinnedVertexBuffer, DrawFlags flags) -> std::optional<DrawCommand> {
        auto resolved = ResolveDrawInputs(_impl.get(), material, mesh, skinnedVertexBuffer);
        if (!resolved) {
            return std::nullopt;
        }

        auto tex = ResolveMaterialTextures(_impl.get(), material);

        uint32_t isSkinned = (skinnedVertexBuffer == BufferHandle::Invalid && (flags & DrawFlags::Skinned) != DrawFlags::None) ? 1u : 0u;

        const auto addr = AddressesOf(*resolved);
        return DrawCommand {
            .instanceData = BuildGPUInstanceData(
                InstanceDataDesc {
                    .resolved                 = &*resolved,
                    .world                    = transform,
                    .prevWorld                = prevTransform,
                    .posAddress               = resolved->posAddr.Get(),
                    .tangentFrameAddress      = resolved->frameAddr.Get(),
                    .surfaceAddress           = resolved->surfaceAddr.Get(),
                    .indices                  = tex,
                    .bakedIrradianceTex     = OptionalTextureIndex(_impl.get(), material.bakedIrradianceMap),
                    .bakedIrradianceScale   = material.bakedIrradianceScale,
                    .alphaMode                = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                    .isBackground             = (flags & DrawFlags::Background) != DrawFlags::None,
                    .isSkinned                = isSkinned != 0u,
                    .doubleSided              = material.doubleSided,
                    .unlit                    = material.unlit,
                    .hasEmissive              = material.bakedIrradianceMap != TextureHandle::Invalid || material.emissiveMap != TextureHandle::Invalid ||
                                                material.emissiveFactor.x > 0.0f || material.emissiveFactor.y > 0.0f || material.emissiveFactor.z > 0.0f,
                    .vertexCount              = resolved->posMesh->vertexCount,
                    .indexCount               = mesh.indexCount,
                    .jointOffset              = jointOffset,
                    .cullRadius               = cullRadius,
                    .metallicFactor           = material.metallicFactor,
                    .roughnessFactor          = material.roughnessFactor,
                    .alphaCutoff              = material.alphaCutoff,
                    .baseColorFactor          = material.baseColorFactor,
                    .emissiveFactor           = material.emissiveFactor,
                    .transmissionFactor       = material.transmissionFactor,
                    .iridescenceFactor        = material.iridescenceFactor,
                    .filmThicknessNm          = material.filmThicknessNm,
                    .filmThicknessMinNm       = material.filmThicknessMinNm,
                    .volumeThicknessM         = material.volumeThicknessM,
                    .ior                      = material.ior,
                    .normalScale              = material.normalScale,
                    .transmissionTex          = OptionalTextureIndex(_impl.get(), material.transmissionMap),
                    .filmThicknessTex         = OptionalTextureIndex(_impl.get(), material.filmThicknessMap),
                    .iridescenceTex           = OptionalTextureIndex(_impl.get(), material.iridescenceMap),
                    .volumeThicknessTex       = OptionalTextureIndex(_impl.get(), material.volumeThicknessMap),
                    .clearcoatFactor          = material.clearcoatFactor,
                    .clearcoatRoughnessFactor = material.clearcoatRoughnessFactor,
                    .clearcoatNormalScale     = material.clearcoatNormalScale,
                    .clearcoatTex             = OptionalTextureIndex(_impl.get(), material.clearcoatMap),
                    .clearcoatRoughnessTex    = OptionalTextureIndex(_impl.get(), material.clearcoatRoughnessMap),
                    .clearcoatNormalTex       = OptionalTextureIndex(_impl.get(), material.clearcoatNormalMap),
                    .anisotropyStrength       = material.anisotropyStrength,
                    .anisotropyRotation       = material.anisotropyRotation,
                    .anisotropyTex            = OptionalTextureIndex(_impl.get(), material.anisotropyMap),
                    .sheenColorFactor         = material.sheenColorFactor,
                    .sheenRoughnessFactor     = material.sheenRoughnessFactor,
                    .sheenColorTex            = OptionalTextureIndex(_impl.get(), material.sheenColorMap),
                    .sheenRoughnessTex        = OptionalTextureIndex(_impl.get(), material.sheenRoughnessMap),
                    .occlusionTex             = OptionalTextureIndex(_impl.get(), material.occlusionMap),
                    .occlusionStrength        = material.occlusionStrength,
                    .specularEnabled          = material.specularEnabled,
                    .environmentReflectionEnabled  = material.environmentReflectionEnabled,
                    .environmentReflectionAlphaMask = material.environmentReflectionAlphaMask,
                    .environmentReflectionFactor = material.environmentReflectionFactor,
                    .specularFactor           = material.specularFactor,
                    .specularColorFactor      = material.specularColorFactor,
                    .specularTex              = OptionalTextureIndex(_impl.get(), material.specularMap),
                    .specularTexMode          = material.specularTexMode,
                    .textureSamplers          = material.textureSamplers,
                    .textureTransforms        = material.textureTransforms,
                }
            ),
            .material            = resolved->material,
            .prePassMaterial     = resolved->prePassMaterial,
            .blasAddress         = addr.blasAddress,
            .skinPosAddress      = addr.skinPosAddress,
            .skinFrameAddress    = addr.skinFrameAddress,
            .skinDataAddress     = addr.skinDataAddress,
            .skinnedVertexBuffer = skinnedVertexBuffer,
            .jointOffset         = jointOffset,
            .morphOffset         = 0,
            .activeMorphCount    = 0,
            .morphWeights        = {},
            .flags               = flags
        };
    };

    const auto eyeDraw = MakeCommand(
        eyeMaterial, eyeMesh, params.eyeParams.transform, params.eyeParams.prevTransform, params.eyeParams.cullRadius, params.eyeParams.jointOffset,
        params.eyeParams.skinnedVertexBuffer, params.eyeParams.flags
    );
    if (!eyeDraw) [[unlikely]] {
        return;
    }

    CSGDrawCommand csgCmd {};
    csgCmd.eyeDraw = *eyeDraw;

    for (const auto& cutter: params.cutters) {
        auto cutCmd = MakeCommand(
            cutter.material, cutter.mesh, cutter.transform, cutter.prevTransform, cutter.cullRadius, cutter.jointOffset, cutter.skinnedVertexBuffer,
            cutter.flags
        );

        if (!cutCmd) [[unlikely]] {
            return;
        }
        csgCmd.cutters.push_back({.draw = *cutCmd, .instanceIdx = 0, .operation = cutter.operation});
    }

    _impl->queues.CsgDraws().push_back(std::move(csgCmd));
}

void RenderContext::DrawDecal(const DecalParams& params) noexcept {
    _impl->queues.Decals().push_back(
        {.transform    = params.transform,
         .invTransform = params.invTransform,
         .albedoIndex  = params.albedoMap != TextureHandle::Invalid ? _impl->textureManager.GetBindlessIndex(params.albedoMap) : 1,
         .normalIndex  = params.normalMap != TextureHandle::Invalid ? _impl->textureManager.GetBindlessIndex(params.normalMap) : 2,
         .roughness    = params.roughness,
         .metallic     = params.metallic}
    );
}

} // namespace ZHLN

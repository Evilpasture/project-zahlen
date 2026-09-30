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

// common.slang reads eight four-bit sampler codes per uint, in material slot
// order. The sampler bank is laid out as S * 3 + T (invalid modes use Repeat).
[[nodiscard]] constexpr auto EncodeMaterialSamplerWord(const MaterialSamplerAddresses& addresses, size_t first) noexcept -> uint32_t {
    uint32_t packed = 0;
    for (size_t i = 0; i < 8 && first + i < addresses.size(); ++i) {
        const auto s = static_cast<uint32_t>(addresses[first + i].s);
        const auto t = static_cast<uint32_t>(addresses[first + i].t);
        const uint32_t code = (s < 3 && t < 3) ? s * 3 + t : 0;
        packed |= code << (i * 4);
    }
    return packed;
}

static_assert(static_cast<size_t>(MaterialTextureSlot::Count) <= 16);
// Pin the shader encoding here, not in the public Material representation.
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

struct ResolvedMeshMaterial {
    NativeMesh*     posMesh         = nullptr;
    NativeMesh*     frameMesh       = nullptr;
    NativeMesh*     surfaceMesh     = nullptr;
    NativeMesh*     finalPosMesh    = nullptr;
    NativeMesh*     skinMesh        = nullptr;
    NativeMesh*     indexMesh       = nullptr;
    NativeMaterial* material        = nullptr;
    NativeMaterial* prePassMaterial = nullptr;
    VkDeviceAddress posAddr         = 0;
    VkDeviceAddress frameAddr       = 0;
    VkDeviceAddress surfaceAddr     = 0;

    VkDeviceAddress meshletAddr       = 0;
    VkDeviceAddress meshletVertexAddr = 0;
    VkDeviceAddress meshletTriAddr    = 0;
    uint32_t        meshletCount      = 0;
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

[[nodiscard]] uint32_t FilmTextureIndex(RenderContext::Impl* impl, TextureHandle handle) noexcept {
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

    uint32_t alphaMode   = 0;
    bool     isViewmodel = false;
    bool     isSkinned   = false;
    bool     doubleSided = false;

    uint32_t vertexCount      = 0;
    uint32_t indexCount       = 0;
    uint32_t jointOffset      = 0;
    uint32_t morphOffset      = 0;
    uint32_t activeMorphCount = 0;

    float cullRadius      = 0.0f;
    float metallicFactor  = 0.0f;
    float roughnessFactor = 1.0f;
    float alphaCutoff     = 0.0f;

    std::array<float, 3> localCenter     = {};
    std::array<float, 4> morphWeights    = {};
    std::array<float, 4> baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissiveFactor  = {0.0f, 0.0f, 0.0f, 1.0f};

    float transmissionFactor = 0.0f;
    float iridescenceFactor  = 0.0f;
    float filmThicknessNm    = 0.0f;
    float filmThicknessMinNm = 0.0f;
    float volumeThicknessM   = 0.0f;
    float ior                = 1.5f;
    float normalScale        = 1.0f;
    uint32_t filmThicknessTex = kNoFilmTexture;
    uint32_t iridescenceTex   = kNoFilmTexture;
    uint32_t volumeThicknessTex = kNoFilmTexture;
    float    clearcoatFactor          = 0.0f;
    float    clearcoatRoughnessFactor = 0.0f;
    float    clearcoatNormalScale     = 1.0f;
    uint32_t clearcoatTex             = kNoFilmTexture;
    uint32_t clearcoatRoughnessTex    = kNoFilmTexture;
    uint32_t clearcoatNormalTex       = kNoFilmTexture;
    float    anisotropyStrength      = 0.0f;
    float    anisotropyRotation      = 0.0f;
    uint32_t anisotropyTex           = kNoFilmTexture;
    std::array<float, 3> sheenColorFactor {};
    float sheenRoughnessFactor = 0.0f;
    uint32_t sheenColorTex = kNoFilmTexture;
    uint32_t sheenRoughnessTex = kNoFilmTexture;
    uint32_t occlusionTex = kNoFilmTexture;
    float occlusionStrength = 1.0f;
    MaterialSamplerAddresses textureSamplers {};
    MaterialTextureTransforms textureTransforms {};
};

[[nodiscard]] inline auto BuildGPUInstanceData(const InstanceDataDesc& desc) noexcept -> InstanceData {
    const ResolvedMeshMaterial* res = desc.resolved;

    const uint32_t isViewmodel = desc.isViewmodel ? 1u : 0u;
    const uint32_t isSkinned   = desc.isSkinned ? 1u : 0u;
    const uint32_t doubleSided = desc.doubleSided ? 1u : 0u;
    const float    clampedT    = std::clamp(desc.transmissionFactor, 0.0f, 1.0f);
    const uint32_t transmission8 = static_cast<uint32_t>(clampedT * 255.0f + 0.5f);

    std::array<float, 4> emissive = desc.emissiveFactor;
    std::array<float, 4> baseColor = desc.baseColorFactor;
    float                alphaCutoff = desc.alphaCutoff;
    float                metallic = desc.metallicFactor;
    uint32_t             paddingCenter = 0;
    uint32_t             paddingMeshlet = 0;
    if (transmission8 != 0) {
        emissive[0] = desc.normalScale;
        emissive[3] = desc.iridescenceFactor;
        alphaCutoff = desc.filmThicknessNm;
        metallic    = desc.ior;
        baseColor[3] = desc.volumeThicknessM;
        const uint32_t filmMin = static_cast<uint32_t>(std::clamp(desc.filmThicknessMinNm, 0.0f, 65535.0f));
        paddingCenter  = (desc.volumeThicknessTex << 16) | (desc.filmThicknessTex & kNoFilmTexture);
        paddingMeshlet = (filmMin << 16) | (desc.iridescenceTex & kNoFilmTexture);
    } else {
        const uint32_t coat8 = static_cast<uint32_t>(std::clamp(desc.clearcoatFactor, 0.0f, 1.0f) * 255.0f + 0.5f);
        if (coat8 != 0) {
            const uint32_t scale8 = static_cast<uint32_t>(std::clamp(desc.clearcoatNormalScale * 64.0f, 0.0f, 255.0f) + 0.5f);
            emissive[3]           = desc.clearcoatRoughnessFactor;
            paddingCenter         = (desc.clearcoatRoughnessTex << 16) | (desc.clearcoatTex & kNoFilmTexture);
            paddingMeshlet        = (scale8 << 24) | (coat8 << 16) | (desc.clearcoatNormalTex & kNoFilmTexture);
        }
    }

    // glTF: uv' = offset + rotation * scale * uv. Keep the per-reference
    // matrices independent (one image can be used in several material slots).
    std::array<JPH::Vec4, static_cast<size_t>(MaterialTextureSlot::Count)> uvRow0;
    std::array<JPH::Vec4, static_cast<size_t>(MaterialTextureSlot::Count)> uvRow1;
    for (size_t slot = 0; slot < uvRow0.size(); ++slot) {
        const auto& transform = desc.textureTransforms[slot];
        const float c = transform.rotation == 0.0f ? 1.0f : std::cos(transform.rotation);
        const float s = transform.rotation == 0.0f ? 0.0f : std::sin(transform.rotation);
        uvRow0[slot] = JPH::Vec4(c * transform.scale[0], -s * transform.scale[1], transform.offset[0], 0.0f);
        uvRow1[slot] = JPH::Vec4(s * transform.scale[0], c * transform.scale[1], transform.offset[1], static_cast<float>(transform.texCoord));
    }

    return InstanceData {
        .world               = desc.world,
        .prevWorld           = desc.prevWorld,
        .posAddress          = desc.posAddress,
        .tangentFrameAddress = desc.tangentFrameAddress,
        .surfaceAddress      = desc.surfaceAddress,
        .skinAddress         = (res != nullptr && res->skinMesh != nullptr) ? res->skinMesh->vboAddress : 0ull,
        .iboAddress          = (res != nullptr && res->indexMesh != nullptr) ? res->indexMesh->vboAddress : 0ull,
        .vertexCount         = desc.vertexCount,
        .indexCount          = desc.indexCount,
        .texIndices0         = (desc.indices.normal << 16) | (desc.indices.albedo & 0xFFFFu),
        .texIndices1         = (desc.indices.emissive << 16) | (desc.indices.pbr & 0xFFFFu),
        .cullRadius          = desc.cullRadius,
        .metallicFactor      = metallic,
        .roughnessFactor     = desc.roughnessFactor,
        .alphaCutoff         = alphaCutoff,
        // Bit 9 tells the task shader to keep back-facing meshlets of a
        // double-sided material. Rasterizer CullNone cannot restore a meshlet
        // rejected by the task stage.
        .flags                = (transmission8 << 24) | (isViewmodel << 16) | (doubleSided << 9) | (isSkinned << 8) | (desc.alphaMode & 0xFFu),
        .jointOffset          = desc.jointOffset,
        .morphOffset          = desc.morphOffset,
        .activeMorphCount     = desc.activeMorphCount,
        .localCenter          = desc.localCenter,
        ._paddingCenter       = paddingCenter,
        .morphWeights         = desc.morphWeights,
        .baseColorFactor      = baseColor,
        .emissiveFactor       = emissive,
        .meshletAddress       = (res != nullptr) ? res->meshletAddr : 0ull,
        .meshletVertexAddress = (res != nullptr) ? res->meshletVertexAddr : 0ull,
        .meshletTriAddress    = (res != nullptr) ? res->meshletTriAddr : 0ull,
        .meshletCount         = (res != nullptr) ? res->meshletCount : 0u,
        ._paddingMeshlet      = paddingMeshlet,
        .anisotropyStrength   = std::clamp(desc.anisotropyStrength, 0.0f, 1.0f),
        .anisotropyRotation   = desc.anisotropyRotation,
        .anisotropyTexIndex   = desc.anisotropyTex,
        .samplerCodes0        = PackMaterialSamplerAddresses(desc.textureSamplers, 0),
        .samplerCodes1        = PackMaterialSamplerAddresses(desc.textureSamplers, 8),
        .sheenParams        = {desc.sheenColorFactor[0], desc.sheenColorFactor[1], desc.sheenColorFactor[2], std::clamp(desc.sheenRoughnessFactor, 0.0f, 1.0f)},
        .sheenColorTexIndex = desc.sheenColorTex,
        .sheenRoughnessTexIndex = desc.sheenRoughnessTex,
        .occlusionTexIndex      = desc.occlusionTex,
        .occlusionStrength      = std::clamp(desc.occlusionStrength, 0.0f, 1.0f),
        .uvRow0                 = uvRow0,
        .uvRow1                 = uvRow1,
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
    // A non-invalid scratch handle can still be stale or undersized. Never
    // enqueue a draw that could read uninitialized positions or tangent frames.
    if (res.finalPosMesh == nullptr) [[unlikely]] {
        return std::nullopt;
    }
    if (skinnedVertexBuffer != Invalid &&
        (res.finalPosMesh->vertexCount < posMesh->vertexCount ||
         res.finalPosMesh->buffer.Size() < static_cast<size_t>(res.finalPosMesh->vertexCount) *
                                               (sizeof(VertexPosition) + sizeof(VertexTangentFrame)))) [[unlikely]] {
        return std::nullopt;
    }

    res.posAddr     = res.finalPosMesh->vboAddress;
    res.frameAddr   = res.frameMesh != nullptr ? res.frameMesh->vboAddress : 0;
    res.surfaceAddr = res.surfaceMesh->vboAddress;

    if (MeshletsUsable(mesh, skinnedVertexBuffer)) {
        auto* meshletMesh = impl->geometry.Resolve(mesh.meshletBuffer);
        auto* meshletVtx  = impl->geometry.Resolve(mesh.meshletVertexBuffer);
        auto* meshletTri  = impl->geometry.Resolve(mesh.meshletTriBuffer);

        if (meshletMesh != nullptr && meshletVtx != nullptr && meshletTri != nullptr) {
            res.meshletAddr       = meshletMesh->vboAddress;
            res.meshletVertexAddr = meshletVtx->vboAddress;
            res.meshletTriAddr    = meshletTri->vboAddress;
            res.meshletCount      = mesh.meshletCount;
        }
    }

    // Persistently mapped debug triangles store positions then surfaces in
    // one buffer. Skinned geometry stores positions then animated frames in
    // scratch; its UVs/color continue to use the original surface buffer.
    if (res.posMesh == res.surfaceMesh) {
        res.surfaceAddr += RenderContext::Impl::kMaxDebugVertices * sizeof(VertexPosition);
    }
    if (skinnedVertexBuffer != Invalid) {
        res.frameAddr = res.finalPosMesh->vboAddress + (res.finalPosMesh->vertexCount * sizeof(VertexPosition));
    }

    return res;
}

}

uint32_t PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept {
    return EncodeMaterialSamplerWord(addresses, first);
}

void RenderContext::Impl::FlushLineQueue() {
    activeLineVertexCount = 0;

    if (queues.Lines().empty() || !linePipeline.Valid()) {
        return;
    }

    constexpr uint32_t maxLineVerts   = kMaxLineVertices;
    uint32_t           totalLineVerts = std::min(static_cast<uint32_t>(queues.Lines().size() * 2), maxLineVerts);

    auto  mappedRegion = frames.lineVbos[presenter.frameIndex].Map(allocator.Get());
    auto* basePosPtr   = static_cast<VertexPosition*>(mappedRegion.data);
    if (basePosPtr == nullptr) return;
    auto* baseSurfacePtr = reinterpret_cast<VertexSurface*>(basePosPtr + maxLineVerts);

    uint32_t vertIdx = 0;
    for (const auto& line: queues.Lines()) {
        if (vertIdx + 2 > totalLineVerts) {
            break;
        }

        basePosPtr[vertIdx]  = {.position = {line.start.GetX(), line.start.GetY(), line.start.GetZ()}};
        baseSurfacePtr[vertIdx] = {
            .uv    = Math::PackUV(0.0f, 0.0f),
            .color = Math::PackColor(line.colorStart.GetX(), line.colorStart.GetY(), line.colorStart.GetZ(), line.colorStart.GetW()),
            .uv1   = Math::PackUV(0.0f, 0.0f)
        };
        vertIdx++;

        basePosPtr[vertIdx]  = {.position = {line.end.GetX(), line.end.GetY(), line.end.GetZ()}};
        baseSurfacePtr[vertIdx] = {
            .uv    = Math::PackUV(1.0f, 1.0f),
            .color = Math::PackColor(line.colorEnd.GetX(), line.colorEnd.GetY(), line.colorEnd.GetZ(), line.colorEnd.GetW()),
            .uv1   = Math::PackUV(1.0f, 1.0f)
        };
        vertIdx++;
    }

    activeLineVertexCount = vertIdx;

    auto lineInstanceIdx = static_cast<uint32_t>(queues.Draws().size());
    lineInstanceId       = lineInstanceIdx;

    const Vk::BufferSlice lineBuffer {frames.lineVbos[presenter.frameIndex], frames.lineVboAddresses[presenter.frameIndex]};
    const VkDeviceSize    posBytes    = static_cast<VkDeviceSize>(maxLineVerts) * sizeof(VertexPosition);
    const auto            positions   = lineBuffer.Subspan(0, posBytes);
    const auto            surfaces    = lineBuffer.Subspan(posBytes, static_cast<VkDeviceSize>(maxLineVerts) * sizeof(VertexSurface));
    const VkDeviceAddress posAddr     = positions.Address();
    const VkDeviceAddress surfaceAddr = surfaces.Address();

    auto  mappedInst = frames.instanceDataBuffers[presenter.frameIndex].Map(allocator.Get());
    auto* dst        = static_cast<InstanceData*>(mappedInst.data);
    if (dst == nullptr) {
        activeLineVertexCount = 0;
        return;
    }

    dst[lineInstanceIdx] = BuildGPUInstanceData(
        InstanceDataDesc {
            .posAddress     = posAddr,
            .surfaceAddress = surfaceAddr,
            .indices        = BindlessIndices {.albedo = 1, .normal = 2, .pbr = 0, .emissive = 1},
            .alphaMode      = 2,
            .vertexCount    = vertIdx,
            .cullRadius     = 10000.0f,
        }
    );

    queues.Lines().clear();
}


void RenderContext::Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept {
    using enum DrawFlags;
    using enum BufferHandle;

    auto resolved = ResolveDrawInputs(_impl.get(), material, mesh, params.skinnedVertexBuffer);
    if (!resolved) [[unlikely]] {
        static uint32_t s_WarnCount = 0;
        if (s_WarnCount++ < 5) {
            ZHLN::Log("WARNING: RenderContext::Draw skipped draw call with invalid mesh, material, or skinned scratch handle.");
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

    _impl->queues.Draws().push_back(
        {.instanceData = BuildGPUInstanceData(
             InstanceDataDesc {
                 .resolved                 = &*resolved,
                 .world                    = params.transform,
                 .prevWorld                = params.prevTransform,
                 .posAddress               = resolved->posAddr,
                 .tangentFrameAddress      = resolved->frameAddr,
                 .surfaceAddress           = resolved->surfaceAddr,
                 .indices                  = tex,
                 .alphaMode                = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                 .isViewmodel              = isViewmodel != 0u,
                 .isSkinned                = isSkinned != 0u,
                 .doubleSided              = material.doubleSided,
                 .vertexCount              = resolved->posMesh->vertexCount,
                 .indexCount               = mesh.indexCount,
                 .jointOffset              = params.jointOffset,
                 .morphOffset              = params.morphOffset,
                 .activeMorphCount         = activeMorphCount,
                 .cullRadius               = params.cullRadius,
                 .metallicFactor           = params.metallic >= 0.0f ? params.metallic : material.metallicFactor,
                 .roughnessFactor          = params.roughness >= 0.0f ? params.roughness : material.roughnessFactor,
                 .alphaCutoff              = material.alphaCutoff,
                 .localCenter              = {params.localCenter[0], params.localCenter[1], params.localCenter[2]},
                 .morphWeights             = params.morphWeights,
                 .baseColorFactor          = (params.colorOverride[3] >= 0.0f) ? params.colorOverride : material.baseColorFactor,
                 .emissiveFactor           = (params.emissiveOverride[3] >= 0.0f) ? params.emissiveOverride : material.emissiveFactor,
                 .transmissionFactor       = material.transmissionFactor,
                 .iridescenceFactor        = material.iridescenceFactor,
                 .filmThicknessNm          = material.filmThicknessNm,
                 .filmThicknessMinNm       = material.filmThicknessMinNm,
                 .volumeThicknessM         = material.volumeThicknessM,
                 .ior                      = material.ior,
                 .normalScale              = material.normalScale,
                 .filmThicknessTex         = FilmTextureIndex(_impl.get(), material.filmThicknessMap),
                 .iridescenceTex           = FilmTextureIndex(_impl.get(), material.iridescenceMap),
                 .volumeThicknessTex       = FilmTextureIndex(_impl.get(), material.volumeThicknessMap),
                 .clearcoatFactor          = material.clearcoatFactor,
                 .clearcoatRoughnessFactor = material.clearcoatRoughnessFactor,
                 .clearcoatNormalScale     = material.clearcoatNormalScale,
                 .clearcoatTex             = FilmTextureIndex(_impl.get(), material.clearcoatMap),
                 .clearcoatRoughnessTex    = FilmTextureIndex(_impl.get(), material.clearcoatRoughnessMap),
                 .clearcoatNormalTex       = FilmTextureIndex(_impl.get(), material.clearcoatNormalMap),
                 .anisotropyStrength       = material.anisotropyStrength,
                 .anisotropyRotation       = material.anisotropyRotation,
                 .anisotropyTex            = FilmTextureIndex(_impl.get(), material.anisotropyMap),
                 .sheenColorFactor         = material.sheenColorFactor,
                 .sheenRoughnessFactor     = material.sheenRoughnessFactor,
                 .sheenColorTex            = FilmTextureIndex(_impl.get(), material.sheenColorMap),
                 .sheenRoughnessTex        = FilmTextureIndex(_impl.get(), material.sheenRoughnessMap),
                 .occlusionTex             = FilmTextureIndex(_impl.get(), material.occlusionMap),
                 .occlusionStrength        = material.occlusionStrength,
                 .textureSamplers          = material.textureSamplers,
                 .textureTransforms        = material.textureTransforms,
             }
         ),
         .material            = resolved->material,
         .prePassMaterial     = resolved->prePassMaterial,
         .posMesh             = resolved->posMesh,
         .frameMesh           = resolved->frameMesh,
         .skinMesh            = resolved->skinMesh,
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

        return DrawCommand {
            .instanceData = BuildGPUInstanceData(
                InstanceDataDesc {
                    .resolved                 = &*resolved,
                    .world                    = transform,
                    .prevWorld                = prevTransform,
                    .posAddress               = resolved->posAddr,
                    .tangentFrameAddress      = resolved->frameAddr,
                    .surfaceAddress           = resolved->surfaceAddr,
                    .indices                  = tex,
                    .alphaMode                = static_cast<uint32_t>(material.alphaMode) & 0xFFu,
                    .isSkinned                = isSkinned != 0u,
                    .doubleSided              = material.doubleSided,
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
                    .filmThicknessTex         = FilmTextureIndex(_impl.get(), material.filmThicknessMap),
                    .iridescenceTex           = FilmTextureIndex(_impl.get(), material.iridescenceMap),
                    .volumeThicknessTex       = FilmTextureIndex(_impl.get(), material.volumeThicknessMap),
                    .clearcoatFactor          = material.clearcoatFactor,
                    .clearcoatRoughnessFactor = material.clearcoatRoughnessFactor,
                    .clearcoatNormalScale     = material.clearcoatNormalScale,
                    .clearcoatTex             = FilmTextureIndex(_impl.get(), material.clearcoatMap),
                    .clearcoatRoughnessTex    = FilmTextureIndex(_impl.get(), material.clearcoatRoughnessMap),
                    .clearcoatNormalTex       = FilmTextureIndex(_impl.get(), material.clearcoatNormalMap),
                    .anisotropyStrength       = material.anisotropyStrength,
                    .anisotropyRotation       = material.anisotropyRotation,
                    .anisotropyTex            = FilmTextureIndex(_impl.get(), material.anisotropyMap),
                    .textureSamplers          = material.textureSamplers,
                }
            ),
            .material            = resolved->material,
            .prePassMaterial     = resolved->prePassMaterial,
            .posMesh             = resolved->posMesh,
            .frameMesh           = resolved->frameMesh,
            .skinMesh            = resolved->skinMesh,
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
        // Missing geometry invalidates the whole boolean, not just this cutter.
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

}

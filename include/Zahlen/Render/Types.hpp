// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Render/Handles.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ZHLN {

// glTF sampler wrapping is attached to a texture *reference*, not its image:
// multiple texture objects may share the same image with different S/T modes.
enum class TextureWrap : uint8_t { Repeat = 0, ClampToEdge = 1, MirroredRepeat = 2 };
struct TextureSamplerAddress {
    TextureWrap    s                                                       = TextureWrap::Repeat;
    TextureWrap    t                                                       = TextureWrap::Repeat;
    constexpr bool operator==(const TextureSamplerAddress&) const noexcept = default;
};

// Each material texture reference can choose its own S/T wrap modes.
enum class MaterialTextureSlot : uint8_t {
    Albedo,
    Normal,
    Pbr,
    Emissive,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormal,
    Anisotropy,
    Iridescence,
    FilmThickness,
    VolumeThickness,
    SheenColor,
    SheenRoughness,
    Occlusion,
    Transmission,
    Count
};
inline constexpr uint32_t kMaterialSamplerVariantCount = 9; // Three S modes x three T modes.
using MaterialSamplerAddresses                         = std::array<TextureSamplerAddress, static_cast<size_t>(MaterialTextureSlot::Count)>;

// glTF textureInfo, not the image, owns the UV transform and set selection.
// Apply offset + rotation * scale to TEXCOORD_0 or TEXCOORD_1 per reference.
//
// A float2 is spelled JPH::Float2 here, as everywhere the engine stores one:
// the pod is the storage spelling, Vec2 does not exist in Jolt, and a
// std::array<float, 2> would be a third spelling of the same eight bytes.
struct MaterialTextureTransform {
    JPH::Float2    offset {0.0f, 0.0f};
    JPH::Float2    scale {1.0f, 1.0f};
    float          rotation                                                   = 0.0f; // Radians, counter-clockwise in glTF UV space.
    uint32_t       texCoord                                                   = 0;
    constexpr bool operator==(const MaterialTextureTransform&) const noexcept = default;
};
using MaterialTextureTransforms = std::array<MaterialTextureTransform, static_cast<size_t>(MaterialTextureSlot::Count)>;

struct Mesh {
    using enum BufferHandle;
    BufferHandle posBuffer          = Invalid;
    BufferHandle tangentFrameBuffer = Invalid;
    BufferHandle surfaceBuffer      = Invalid;
    BufferHandle skinBuffer         = Invalid;
    BufferHandle indexBuffer        = Invalid;
    uint32_t     vertexCount        = 0;
    uint32_t     indexCount         = 0;

    BufferHandle meshletBuffer       = Invalid;
    BufferHandle meshletVertexBuffer = Invalid;
    BufferHandle meshletTriBuffer    = Invalid;
    uint32_t     meshletCount        = 0;
};

struct Material {
    PipelineHandle            pipeline                 = PipelineHandle::Invalid;
    PipelineHandle            prePassPipeline          = PipelineHandle::Invalid;
    ResourceGroupHandle       resourceGroup            = ResourceGroupHandle::Invalid;
    BufferHandle              constantBuffer           = BufferHandle::Invalid;
    TextureHandle             albedoMap                = TextureHandle::Invalid;
    TextureHandle             normalMap                = TextureHandle::Invalid;
    TextureHandle             pbrMap                   = TextureHandle::Invalid;
    TextureHandle             emissiveMap              = TextureHandle::Invalid;
    JPH::Float4               baseColorFactor          = {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4               emissiveFactor           = {0.0f, 0.0f, 0.0f, 1.0f};
    float                     metallicFactor           = 1.0f;
    float                     roughnessFactor          = 1.0f;
    float                     alphaCutoff              = 0.5f;
    uint32_t                  alphaMode                = 0;
    bool                      doubleSided              = false;
    bool                      unlit                    = false; // KHR_materials_unlit: base color without lighting.
    float                     transmissionFactor       = 0.0f;
    TextureHandle             transmissionMap          = TextureHandle::Invalid;
    float                     iridescenceFactor        = 0.0f;
    float                     filmThicknessNm          = 0.0f;
    float                     filmThicknessMinNm       = 0.0f;
    float                     volumeThicknessM         = 0.0f;
    float                     ior                      = 1.5f;
    float                     normalScale              = 1.0f;
    TextureHandle             filmThicknessMap         = TextureHandle::Invalid;
    TextureHandle             iridescenceMap           = TextureHandle::Invalid;
    TextureHandle             volumeThicknessMap       = TextureHandle::Invalid;
    float                     clearcoatFactor          = 0.0f;
    float                     clearcoatRoughnessFactor = 0.0f;
    float                     clearcoatNormalScale     = 1.0f;
    TextureHandle             clearcoatMap             = TextureHandle::Invalid;
    TextureHandle             clearcoatRoughnessMap    = TextureHandle::Invalid;
    TextureHandle             clearcoatNormalMap       = TextureHandle::Invalid;
    float                     anisotropyStrength       = 0.0f;
    float                     anisotropyRotation       = 0.0f; // Radians about the surface normal, from the tangent.
    TextureHandle             anisotropyMap            = TextureHandle::Invalid;
    JPH::Float3               sheenColorFactor {0.0f, 0.0f, 0.0f};
    float                     sheenRoughnessFactor = 0.0f;
    TextureHandle             sheenColorMap        = TextureHandle::Invalid;
    TextureHandle             sheenRoughnessMap    = TextureHandle::Invalid;
    TextureHandle             occlusionMap         = TextureHandle::Invalid;
    float                     occlusionStrength    = 1.0f;
    MaterialSamplerAddresses  textureSamplers {}; // Repeat/Repeat for non-glTF materials.
    MaterialTextureTransforms textureTransforms {};
};

// The lane types are the only spellings a float2/float3/float4 has in this
// header. A std::array<float, N> here would be a second spelling of the same
// bytes, and every crossing into the GPU layout -- the one place these values
// end up -- would need a hand-written conversion for it. The pods are the same
// size and alignment the arrays had, so this changes no struct's layout.
static_assert(
    sizeof(JPH::Float2) == 8 && alignof(JPH::Float2) == 4 && sizeof(JPH::Float3) == 12 && alignof(JPH::Float3) == 4 && sizeof(JPH::Float4) == 16 &&
        alignof(JPH::Float4) == 4,
    "the lane pods are the storage spelling; their geometry is the ABI"
);

enum class DrawFlags : uint32_t {
    None            = 0,
    ExcludeFromTLAS = 1 << 0,
    Skinned         = 1 << 1,
    VisibleInMain   = 1 << 2,
    VisibleInShadow = 1 << 3,
    Hidden          = 1 << 4,
    Viewmodel       = 1 << 5,
};

enum class CSGOperation : uint8_t { Difference = 0, Union = 1, Intersection = 2 };

struct CSGModifier {
    CSGOperation operation;
    std::string  operand_name;
};

struct MaterialDesc {
    bool doubleSided   = false;
    bool unlit         = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    uint32_t      alphaMode          = 0;
    float         alphaCutoff        = 0.5f;
    float         metallic           = 1.0f;
    float         roughness          = 1.0f;
    JPH::Float4   baseColor          = {1.0f, 1.0f, 1.0f, 1.0f};
    JPH::Float4   emissive           = {0.0f, 0.0f, 0.0f, 1.0f};
    float         transmissionFactor = 0.0f;
    TextureHandle transmissionMap    = TextureHandle::Invalid;
    float         iridescenceFactor  = 0.0f;
    float         filmThicknessNm    = 0.0f;
    float         filmThicknessMinNm = 0.0f;
    float         volumeThicknessM   = 0.0f;
    float         ior                = 1.5f;
    float         normalScale        = 1.0f;

    TextureHandle             albedoMap                = TextureHandle::Invalid;
    TextureHandle             normalMap                = TextureHandle::Invalid;
    TextureHandle             pbrMap                   = TextureHandle::Invalid;
    TextureHandle             emissiveMap              = TextureHandle::Invalid;
    TextureHandle             filmThicknessMap         = TextureHandle::Invalid;
    TextureHandle             iridescenceMap           = TextureHandle::Invalid;
    TextureHandle             volumeThicknessMap       = TextureHandle::Invalid;
    float                     clearcoatFactor          = 0.0f;
    float                     clearcoatRoughnessFactor = 0.0f;
    float                     clearcoatNormalScale     = 1.0f;
    TextureHandle             clearcoatMap             = TextureHandle::Invalid;
    TextureHandle             clearcoatRoughnessMap    = TextureHandle::Invalid;
    TextureHandle             clearcoatNormalMap       = TextureHandle::Invalid;
    float                     anisotropyStrength       = 0.0f;
    float                     anisotropyRotation       = 0.0f; // KHR_materials_anisotropy radians.
    TextureHandle             anisotropyMap            = TextureHandle::Invalid;
    JPH::Float3               sheenColorFactor {0.0f, 0.0f, 0.0f};
    float                     sheenRoughnessFactor = 0.0f;
    TextureHandle             sheenColorMap        = TextureHandle::Invalid;
    TextureHandle             sheenRoughnessMap    = TextureHandle::Invalid;
    TextureHandle             occlusionMap         = TextureHandle::Invalid;
    float                     occlusionStrength    = 1.0f;
    MaterialSamplerAddresses  textureSamplers {};
    MaterialTextureTransforms textureTransforms {};

    static MaterialDesc Basic(
        JPH::Float4 color = {1.0f, 1.0f, 1.0f, 1.0f}, float roughness = 0.5f, float metallic = 0.0f, bool twoSided = false
    ) {
        return MaterialDesc {.doubleSided = twoSided, .metallic = metallic, .roughness = roughness, .baseColor = color};
    }
    static MaterialDesc Transparent(JPH::Float4 color = {1.0f, 1.0f, 1.0f, 1.0f}, float roughness = 0.1f, bool twoSided = false) {
        return MaterialDesc {
            .doubleSided = twoSided, .alphaBlend = true, .alphaMode = 2, .metallic = 0.0f, .roughness = roughness, .baseColor = color
        };
    }
    static MaterialDesc Unlit(JPH::Float4 color = {1.0f, 1.0f, 1.0f, 1.0f}) {
        return MaterialDesc {.unlit = true, .baseColor = color};
    }
};

struct DrawParams {
    JPH::Mat44  transform        = JPH::Mat44::sIdentity();
    JPH::Mat44  prevTransform    = JPH::Mat44::sIdentity();
    float       cullRadius       = 1.0f;
    JPH::Float3 localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t    jointOffset      = 0;
    uint32_t    morphOffset      = 0;
    uint32_t    activeMorphCount = 0;
    JPH::Float4 morphWeights     = {};
    DrawFlags   flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    JPH::Float4 colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f};
    JPH::Float4 emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f};
};

struct CSGCutterParams {
    Mesh         mesh;
    Material     material;
    JPH::Mat44   transform           = JPH::Mat44::sIdentity();
    JPH::Mat44   prevTransform       = JPH::Mat44::sIdentity();
    float        cullRadius          = 1.0f;
    CSGOperation operation           = CSGOperation::Difference;
    uint32_t     jointOffset         = 0;
    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;
    DrawFlags    flags               = DrawFlags::None;
};

struct CSGDrawParams {
    DrawParams                   eyeParams;
    ZHLN::Array<CSGCutterParams> cutters;
};

struct DecalParams {
    JPH::Mat44    transform    = JPH::Mat44::sIdentity();
    JPH::Mat44    invTransform = JPH::Mat44::sIdentity();
    TextureHandle albedoMap    = TextureHandle::Invalid;
    TextureHandle normalMap    = TextureHandle::Invalid;
    float         roughness    = 0.5f;
    float         metallic     = 0.0f;
};

} // namespace ZHLN

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;

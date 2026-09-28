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
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec4.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ZHLN {

// glTF sampler wrapping is attached to a texture *reference*, not its image:
// multiple texture objects may share the same image with different S/T modes.
enum class TextureWrap : uint8_t { Repeat = 0, ClampToEdge = 1, MirroredRepeat = 2 };
struct TextureSamplerAddress {
    TextureWrap s = TextureWrap::Repeat;
    TextureWrap t = TextureWrap::Repeat;
    constexpr bool operator==(const TextureSamplerAddress&) const noexcept = default;
};

// Must agree with the packed sampler slots in common.slang. Eight four-bit
// indices fit in one uint; the remaining three fit in a second uint.
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
    Count
};
inline constexpr uint32_t kMaterialSamplerVariantCount = 9; // Three S modes x three T modes.
using MaterialSamplerAddresses = std::array<TextureSamplerAddress, static_cast<size_t>(MaterialTextureSlot::Count)>;

[[nodiscard]] constexpr uint32_t PackMaterialSamplerAddresses(const MaterialSamplerAddresses& addresses, size_t first) noexcept {
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

struct Mesh {
    using enum BufferHandle;
    BufferHandle posBuffer   = Invalid;
    BufferHandle attrBuffer  = Invalid;
    BufferHandle skinBuffer  = Invalid;
    BufferHandle indexBuffer = Invalid;
    uint32_t     vertexCount = 0;
    uint32_t     indexCount  = 0;

    BufferHandle meshletBuffer       = Invalid;
    BufferHandle meshletVertexBuffer = Invalid;
    BufferHandle meshletTriBuffer    = Invalid;
    uint32_t     meshletCount        = 0;
};

struct Material {
    PipelineHandle      pipeline           = PipelineHandle::Invalid;
    PipelineHandle      prePassPipeline    = PipelineHandle::Invalid;
    ResourceGroupHandle resourceGroup      = ResourceGroupHandle::Invalid;
    BufferHandle        constantBuffer     = BufferHandle::Invalid;
    TextureHandle       albedoMap          = TextureHandle::Invalid;
    TextureHandle       normalMap          = TextureHandle::Invalid;
    TextureHandle       pbrMap             = TextureHandle::Invalid;
    TextureHandle       emissiveMap        = TextureHandle::Invalid;
    std::array<float, 4> baseColorFactor    = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissiveFactor     = {0.0f, 0.0f, 0.0f, 1.0f};
    float               metallicFactor     = 1.0f;
    float               roughnessFactor    = 1.0f;
    float               alphaCutoff        = 0.5f;
    uint32_t            alphaMode          = 0;
    bool                doubleSided        = false;
    float               transmissionFactor = 0.0f;
    float               iridescenceFactor  = 0.0f;
    float               filmThicknessNm    = 0.0f;
    float               filmThicknessMinNm = 0.0f;
    float               volumeThicknessM   = 0.0f;
    float               ior                = 1.5f;
    float               normalScale        = 1.0f;
    TextureHandle       filmThicknessMap   = TextureHandle::Invalid;
    TextureHandle       iridescenceMap     = TextureHandle::Invalid;
    TextureHandle       volumeThicknessMap = TextureHandle::Invalid;
    float               clearcoatFactor          = 0.0f;
    float               clearcoatRoughnessFactor = 0.0f;
    float               clearcoatNormalScale     = 1.0f;
    TextureHandle       clearcoatMap             = TextureHandle::Invalid;
    TextureHandle       clearcoatRoughnessMap    = TextureHandle::Invalid;
    TextureHandle       clearcoatNormalMap       = TextureHandle::Invalid;
    float               anisotropyStrength      = 0.0f;
    float               anisotropyRotation      = 0.0f; // Radians about the surface normal, from the tangent.
    TextureHandle       anisotropyMap           = TextureHandle::Invalid;
    MaterialSamplerAddresses textureSamplers {}; // Repeat/Repeat for non-glTF materials.
};

static_assert(
    sizeof(std::array<float, 4>) == sizeof(float[4]) && alignof(std::array<float, 4>) == alignof(float[4]),
    "material factors must preserve their four-float ABI"
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

struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44 invTransform;
    JPH::Vec4  extentsAndType;
    JPH::Vec4  colorAndDensity;
    JPH::Vec4  emissiveAndAniso;
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

enum class CSGOperation : uint8_t { Difference = 0, Union = 1, Intersection = 2 };

struct CSGModifier {
    CSGOperation operation;
    std::string  operand_name;
};


struct MaterialDesc {
    bool doubleSided   = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    uint32_t             alphaMode   = 0;
    float                alphaCutoff = 0.5f;
    float                metallic    = 1.0f;
    float                roughness   = 1.0f;
    std::array<float, 4> baseColor   = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissive    = {0.0f, 0.0f, 0.0f, 1.0f};
    float                transmissionFactor = 0.0f;
    float                iridescenceFactor  = 0.0f;
    float                filmThicknessNm    = 0.0f;
    float                filmThicknessMinNm = 0.0f;
    float                volumeThicknessM   = 0.0f;
    float                ior                = 1.5f;
    float                normalScale        = 1.0f;

    TextureHandle albedoMap          = TextureHandle::Invalid;
    TextureHandle normalMap          = TextureHandle::Invalid;
    TextureHandle pbrMap             = TextureHandle::Invalid;
    TextureHandle emissiveMap        = TextureHandle::Invalid;
    TextureHandle filmThicknessMap   = TextureHandle::Invalid;
    TextureHandle iridescenceMap     = TextureHandle::Invalid;
    TextureHandle volumeThicknessMap = TextureHandle::Invalid;
    float         clearcoatFactor          = 0.0f;
    float         clearcoatRoughnessFactor = 0.0f;
    float         clearcoatNormalScale     = 1.0f;
    TextureHandle clearcoatMap          = TextureHandle::Invalid;
    TextureHandle clearcoatRoughnessMap = TextureHandle::Invalid;
    TextureHandle clearcoatNormalMap    = TextureHandle::Invalid;
    float         anisotropyStrength    = 0.0f;
    float         anisotropyRotation    = 0.0f; // KHR_materials_anisotropy radians.
    TextureHandle anisotropyMap         = TextureHandle::Invalid;
    MaterialSamplerAddresses textureSamplers {};
};

struct DrawParams {
    JPH::Mat44           transform        = JPH::Mat44::sIdentity();
    JPH::Mat44           prevTransform    = JPH::Mat44::sIdentity();
    float                cullRadius       = 1.0f;
    std::array<float, 3> localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t             jointOffset      = 0;
    uint32_t             morphOffset      = 0;
    uint32_t             activeMorphCount = 0;
    std::array<float, 4> morphWeights     = {};
    DrawFlags            flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    std::array<float, 4> colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f};
    std::array<float, 4> emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f};
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

}

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;

// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

namespace ZHLN {

// Public buffer role. GeometryManager maps this to Vk::BufferUsage bits.
enum class BufferUsage : uint8_t {
    Vertex,
    Index,
    Storage,
    Uniform,
    SkinScratch, // skinned-mesh deform target: pos + tangent frame, no host upload
};

template <typename T>
concept ValidIndexType = std::same_as<std::remove_cv_t<T>, uint16_t> || std::same_as<std::remove_cv_t<T>, uint32_t>;

struct BufferDesc {
    BufferUsage                usage  = BufferUsage::Storage;
    std::span<const std::byte> data   = {}; // empty = zero-fill / uninitialized
    size_t                     size   = 0;  // used when data is empty
    uint32_t                   stride = 1;  // bytes per element
};

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
    Specular
};
// The slot count is reflected, not sentineled: a Count enumerator would make
// every reflection tool (EnumCount, EnumNames, ForEachEnumerator, EnumToString)
// see a phantom seventeenth slot.
inline constexpr size_t   kMaterialTextureSlotCount    = Reflect::EnumCount<MaterialTextureSlot>();
inline constexpr uint32_t kMaterialSamplerVariantCount = 9; // Three S modes x three T modes.
using MaterialSamplerAddresses                         = std::array<TextureSamplerAddress, kMaterialTextureSlotCount>;

// glTF textureInfo, not the image, owns the UV transform and set selection.
// Apply offset + rotation * scale to TEXCOORD_0 or TEXCOORD_1 per reference.
//
// The two 2-component fields are ZHLN::FVec2: eight bytes, the storage a
// ZHLN::FVec2 holds, and the only 2D type in the engine's vocabulary -- a
// std::array<float, 2> would be a second spelling of the same eight bytes.
struct MaterialTextureTransform {
    ZHLN::FVec2   offset {0.0f, 0.0f};
    ZHLN::FVec2   scale {1.0f, 1.0f};
    float          rotation                                                   = 0.0f; // Radians, counter-clockwise in glTF UV space.
    uint32_t       texCoord                                                   = 0;
    constexpr bool operator==(const MaterialTextureTransform&) const noexcept = default;
};
using MaterialTextureTransforms = std::array<MaterialTextureTransform, kMaterialTextureSlotCount>;

struct MeshletStreams {
    using enum BufferID;
    BufferID meshlets  = Invalid;
    BufferID vertices  = Invalid;
    BufferID triangles = Invalid;
    uint32_t count     = 0;

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return count != 0 && meshlets != Invalid && vertices != Invalid && triangles != Invalid;
    }
};

struct Mesh {
    using enum BufferID;
    BufferID posBuffer          = Invalid;
    BufferID tangentFrameBuffer = Invalid;
    BufferID surfaceBuffer      = Invalid;
    BufferID skinBuffer         = Invalid;
    BufferID indexBuffer        = Invalid;
    uint32_t vertexCount        = 0;
    uint32_t indexCount         = 0;

    BufferID meshletBuffer       = Invalid;
    BufferID meshletVertexBuffer = Invalid;
    BufferID meshletTriBuffer    = Invalid;
    uint32_t meshletCount        = 0;
};

struct Material {
    PipelineID pipeline        = PipelineID::Invalid;
    PipelineID prePassPipeline = PipelineID::Invalid;
    TextureID  albedoMap       = TextureID::Invalid;
    TextureID  normalMap       = TextureID::Invalid;
    TextureID  pbrMap          = TextureID::Invalid;
    TextureID  emissiveMap     = TextureID::Invalid;
    // Authoritative static diffuse irradiance (for example Source VRAD), not
    // AO and not material emission. It is sampled with the mesh's UV1.
    TextureID    bakedIrradianceMap       = TextureID::Invalid;
    float        bakedIrradianceScale     = 1.0f;
    ZHLN::FVec4 baseColorFactor          = {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::FVec4 emissiveFactor           = {0.0f, 0.0f, 0.0f, 1.0f};
    float        metallicFactor           = 1.0f;
    float        roughnessFactor          = 1.0f;
    float        alphaCutoff              = 0.5f;
    uint32_t     alphaMode                = 0;
    bool         doubleSided              = false;
    bool         unlit                    = false; // KHR_materials_unlit: base color without lighting.
    float        transmissionFactor       = 0.0f;
    TextureID    transmissionMap          = TextureID::Invalid;
    float        iridescenceFactor        = 0.0f;
    float        filmThicknessNm          = 0.0f;
    float        filmThicknessMinNm       = 0.0f;
    float        volumeThicknessM         = 0.0f;
    float        ior                      = 1.5f;
    float        normalScale              = 1.0f;
    TextureID    filmThicknessMap         = TextureID::Invalid;
    TextureID    iridescenceMap           = TextureID::Invalid;
    TextureID    volumeThicknessMap       = TextureID::Invalid;
    float        clearcoatFactor          = 0.0f;
    float        clearcoatRoughnessFactor = 0.0f;
    float        clearcoatNormalScale     = 1.0f;
    TextureID    clearcoatMap             = TextureID::Invalid;
    TextureID    clearcoatRoughnessMap    = TextureID::Invalid;
    TextureID    clearcoatNormalMap       = TextureID::Invalid;
    float        anisotropyStrength       = 0.0f;
    float        anisotropyRotation       = 0.0f; // Radians about the surface normal, from the tangent.
    TextureID    anisotropyMap            = TextureID::Invalid;
    ZHLN::FVec3 sheenColorFactor {0.0f, 0.0f, 0.0f};
    float        sheenRoughnessFactor = 0.0f;
    TextureID    sheenColorMap        = TextureID::Invalid;
    TextureID    sheenRoughnessMap    = TextureID::Invalid;
    TextureID    occlusionMap         = TextureID::Invalid;
    float        occlusionStrength    = 1.0f;
    // KHR_materials_specular. Absent, the extension's defaults reproduce the
    // engine's own 0.04 dielectric reflectance exactly, so `specularEnabled`
    // (the extension being declared on this material) is what decides whether
    // the authored response replaces that baseline. A specularColorFactor
    // above 1 is clamped by the importer: the lane the opaque path carries the
    // response in holds a multiple of the 0.04 baseline, and clamping once is
    // what keeps every path agreeing on the same reflectance.
    bool specularEnabled = false;
    // Scale the deferred image-based reflection independently of the glTF
    // specular extension; optionally gate it with base-color texture alpha.
    bool         environmentReflectionEnabled   = false;
    bool         environmentReflectionAlphaMask = false;
    float        environmentReflectionFactor    = 1.0f;
    float        specularFactor                 = 1.0f;
    ZHLN::FVec3 specularColorFactor {1.0f, 1.0f, 1.0f};
    TextureID    specularMap = TextureID::Invalid;
    // Which half of the single KHR_materials_specular texture slot this
    // material's image carries: 0 = specularColorTexture.rgb and
    // specularTexture.a of one shared image, 1 = specularTexture.a only,
    // 2 = specularColorTexture.rgb only. The extension has two texture
    // references but glTF's sampler codes have exactly one nibble left.
    uint32_t                  specularTexMode = 0;
    MaterialSamplerAddresses  textureSamplers {}; // Repeat/Repeat for non-glTF materials.
    MaterialTextureTransforms textureTransforms {};
};

// The lane types are the only spellings a float2/float3/float4 has in this
// header. A std::array<float, N> here would be a second spelling of the same
// bytes, and every crossing into the GPU layout -- the one place these values
// end up -- would need a hand-written conversion for it. The value types are the
// same size and alignment as the Jolt pods they stand in for, so this changes no
// struct's layout.
static_assert(
    sizeof(ZHLN::FVec2) == 8 && alignof(ZHLN::FVec2) == 4 && sizeof(ZHLN::FVec3) == 12 && alignof(ZHLN::FVec3) == 4 && sizeof(ZHLN::FVec4) == 16 &&
        alignof(ZHLN::FVec4) == 4,
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
    // Background layer (Source's 3D-skybox world): rasterized into the shared
    // G-buffer by BackgroundGBufferPass with its clip depth remapped to the
    // far band [kSkyDepthSplit, 1] while the main world uses [0, kSkyDepthSplit]
    // -- Source's depth-range remap, so the map keeps every pixel it covers no
    // matter how close the background geometry sits. Never visible in the
    // shadow passes.
    Background = 1 << 6,
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

    uint32_t     alphaMode          = 0;
    float        alphaCutoff        = 0.5f;
    float        metallic           = 1.0f;
    float        roughness          = 1.0f;
    ZHLN::FVec4 baseColor          = {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::FVec4 emissive           = {0.0f, 0.0f, 0.0f, 1.0f};
    float        transmissionFactor = 0.0f;
    TextureID    transmissionMap    = TextureID::Invalid;
    float        iridescenceFactor  = 0.0f;
    float        filmThicknessNm    = 0.0f;
    float        filmThicknessMinNm = 0.0f;
    float        volumeThicknessM   = 0.0f;
    float        ior                = 1.5f;
    float        normalScale        = 1.0f;

    TextureID albedoMap   = TextureID::Invalid;
    TextureID normalMap   = TextureID::Invalid;
    TextureID pbrMap      = TextureID::Invalid;
    TextureID emissiveMap = TextureID::Invalid;
    // A linear HDR baked-diffuse input. It has a separate semantic from
    // occlusionMap because authored irradiance and ambient occlusion are not
    // interchangeable PBR quantities.
    TextureID    bakedIrradianceMap       = TextureID::Invalid;
    float        bakedIrradianceScale     = 1.0f;
    TextureID    filmThicknessMap         = TextureID::Invalid;
    TextureID    iridescenceMap           = TextureID::Invalid;
    TextureID    volumeThicknessMap       = TextureID::Invalid;
    float        clearcoatFactor          = 0.0f;
    float        clearcoatRoughnessFactor = 0.0f;
    float        clearcoatNormalScale     = 1.0f;
    TextureID    clearcoatMap             = TextureID::Invalid;
    TextureID    clearcoatRoughnessMap    = TextureID::Invalid;
    TextureID    clearcoatNormalMap       = TextureID::Invalid;
    float        anisotropyStrength       = 0.0f;
    float        anisotropyRotation       = 0.0f; // KHR_materials_anisotropy radians.
    TextureID    anisotropyMap            = TextureID::Invalid;
    ZHLN::FVec3 sheenColorFactor {0.0f, 0.0f, 0.0f};
    float        sheenRoughnessFactor = 0.0f;
    TextureID    sheenColorMap        = TextureID::Invalid;
    TextureID    sheenRoughnessMap    = TextureID::Invalid;
    TextureID    occlusionMap         = TextureID::Invalid;
    float        occlusionStrength    = 1.0f;
    // See Material: `specularEnabled` is the extension's presence on this
    // material, not a deviation of the factors from their defaults.
    bool specularEnabled = false;
    // Scale the deferred image-based reflection independently of the glTF
    // specular extension; optionally gate it with base-color texture alpha.
    bool                      environmentReflectionEnabled   = false;
    bool                      environmentReflectionAlphaMask = false;
    float                     environmentReflectionFactor    = 1.0f;
    float                     specularFactor                 = 1.0f;
    ZHLN::FVec3              specularColorFactor {1.0f, 1.0f, 1.0f};
    TextureID                 specularMap     = TextureID::Invalid;
    uint32_t                  specularTexMode = 0;
    MaterialSamplerAddresses  textureSamplers {};
    MaterialTextureTransforms textureTransforms {};

    static MaterialDesc Basic(ZHLN::FVec4 color = {1.0f, 1.0f, 1.0f, 1.0f}, float roughness = 0.5f, float metallic = 0.0f, bool twoSided = false) {
        return MaterialDesc {.doubleSided = twoSided, .metallic = metallic, .roughness = roughness, .baseColor = color};
    }
    static MaterialDesc Transparent(ZHLN::FVec4 color = {1.0f, 1.0f, 1.0f, 1.0f}, float roughness = 0.1f, bool twoSided = false) {
        return MaterialDesc {.doubleSided = twoSided, .alphaBlend = true, .alphaMode = 2, .metallic = 0.0f, .roughness = roughness, .baseColor = color};
    }
    static MaterialDesc Unlit(ZHLN::FVec4 color = {1.0f, 1.0f, 1.0f, 1.0f}) {
        return MaterialDesc {.unlit = true, .baseColor = color};
    }
};

struct DrawParams {
    ZHLN::Mat4x4  transform        = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4  prevTransform    = ZHLN::Mat4x4::Identity();
    float        cullRadius       = 1.0f;
    ZHLN::FVec3 localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t     jointOffset      = 0;
    uint32_t     morphOffset      = 0;
    uint32_t     activeMorphCount = 0;
    ZHLN::FVec4 morphWeights     = {};
    DrawFlags    flags            = DrawFlags::None;

    BufferID skinnedVertexBuffer = BufferID::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    ZHLN::FVec4 colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f};
    ZHLN::FVec4 emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f};

    // Optional ambient cube (+X,-X,+Y,-Y,+Z,-Z linear radiance) for meshes without
    // baked lightmaps. Only read when hasAmbientCube is set.
    bool                        hasAmbientCube = false;
    std::array<ZHLN::FVec4, 6> ambientCube {};
};

struct CSGCutterParams {
    Mesh         mesh;
    Material     material;
    ZHLN::Mat4x4  transform           = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4  prevTransform       = ZHLN::Mat4x4::Identity();
    float        cullRadius          = 1.0f;
    CSGOperation operation           = CSGOperation::Difference;
    uint32_t     jointOffset         = 0;
    BufferID     skinnedVertexBuffer = BufferID::Invalid;
    DrawFlags    flags               = DrawFlags::None;
};

struct CSGDrawParams {
    DrawParams                   eyeParams;
    ZHLN::Array<CSGCutterParams> cutters;
};

struct DecalParams {
    ZHLN::Mat4x4 transform    = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4 invTransform = ZHLN::Mat4x4::Identity();
    TextureID   albedoMap    = TextureID::Invalid;
    TextureID   normalMap    = TextureID::Invalid;
    float       roughness    = 0.5f;
    float       metallic     = 0.0f;
};

} // namespace ZHLN

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;

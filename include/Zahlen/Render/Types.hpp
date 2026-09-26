// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/Types.hpp
//
// Everything a caller hands the renderer or gets back from it, in data form: the
// descriptor structs it fills in to ask for something (a material recipe, one
// draw, a CSG draw, a decal) and the resource structs it receives (a mesh, a
// material). Pure data -- there is no renderer state here, only the vocabulary
// the API is written in.
//
// The opaque handles and the subresource reference live in
// <Zahlen/Render/Handles.hpp>, which this header includes: code that only needs
// to name a texture should not have to compile Jolt to do it.
#pragma once
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/EnumFlags.hpp> // EnableEnumFlags, for DrawFlags
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Render/Handles.hpp> // TextureHandle, BufferHandle, PipelineHandle, ResourceGroupHandle
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec4.h>
#include <array>
#include <cstdint>
#include <string>

namespace ZHLN {

// --- Resources the renderer hands back

struct Mesh {
    using enum BufferHandle;
    BufferHandle posBuffer   = Invalid;
    BufferHandle attrBuffer  = Invalid;
    BufferHandle skinBuffer  = Invalid;
    BufferHandle indexBuffer = Invalid;
    uint32_t     vertexCount = 0;
    uint32_t     indexCount  = 0;

    // --- VK_EXT_mesh_shader meshlet streams
    // The raw position/attribute/index buffers above are deliberately kept:
    // ray tracing BLAS builds (ZHLN_CmdBuildBlas) and the legacy vertex
    // pipeline still consume them. Meshlets are an additional view of the
    // very same vertex pool.
    BufferHandle meshletBuffer       = Invalid; // GPUMeshlet[]
    BufferHandle meshletVertexBuffer = Invalid; // uint32_t[]
    BufferHandle meshletTriBuffer    = Invalid; // uint8_t[] (padded to 4B)
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
    float               baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float               emissiveFactor[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float               metallicFactor     = 1.0f;
    float               roughnessFactor    = 1.0f;
    float               alphaCutoff        = 0.5f;
    uint32_t            alphaMode          = 0;
    // KHR_materials_transmission. Above zero the draw is forward-only: it
    // samples a copy of the lit scene, refracts, and writes the composite.
    // It does not cast a shadow. baseColor alpha is not coverage (glTF leaves
    // it at 1).
    float               transmissionFactor = 0.0f;
    // KHR_materials_iridescence. Thickness is nanometres. A thickness texture
    // lerps filmThicknessMinNm..filmThicknessNm; without one, the max is used.
    float               iridescenceFactor  = 0.0f;
    float               filmThicknessNm    = 0.0f;
    float               filmThicknessMinNm = 0.0f;
    // KHR_materials_volume thickness in metres, before the thickness texture.
    float               volumeThicknessM   = 0.0f;
    float               ior                = 1.5f;
    float               normalScale        = 1.0f;
    TextureHandle       filmThicknessMap   = TextureHandle::Invalid;
    TextureHandle       iridescenceMap     = TextureHandle::Invalid;
    TextureHandle       volumeThicknessMap = TextureHandle::Invalid;
    // KHR_materials_clearcoat. Factor 0 is no lacquer. The coat normal is
    // independent of the base normal; scale is the normal-texture scale.
    float               clearcoatFactor          = 0.0f;
    float               clearcoatRoughnessFactor = 0.0f;
    float               clearcoatNormalScale     = 1.0f;
    TextureHandle       clearcoatMap             = TextureHandle::Invalid;
    TextureHandle       clearcoatRoughnessMap    = TextureHandle::Invalid;
    TextureHandle       clearcoatNormalMap       = TextureHandle::Invalid;
};

// --- Per-draw classification
//
// A bit set on the draw, not on the material: the same mesh/material pair is
// visible in the main pass and invisible to the TLAS. EnableEnumFlags is what
// turns the enum into one (see Core/EnumFlags.hpp) -- the specialization is at
// the bottom of this header.
enum class DrawFlags : uint32_t {
    None            = 0,
    ExcludeFromTLAS = 1 << 0,
    Skinned         = 1 << 1,
    VisibleInMain   = 1 << 2,
    VisibleInShadow = 1 << 3,
    Hidden          = 1 << 4,
    Viewmodel       = 1 << 5,
};

// --- Volumetric volume
//
// The one struct here that is a GPU upload rather than an API argument: it is
// written into a storage buffer and read by the volumetric passes, so its
// layout is held by a static_assert. Jolt's math types are used as the math
// vocabulary of the buffer, which is why this header includes Jolt.
struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44 invTransform;
    JPH::Vec4  extentsAndType;   // xyz = extents, w = type (0=Box, 1=Sphere)
    JPH::Vec4  colorAndDensity;  // xyz = color, w = density
    JPH::Vec4  emissiveAndAniso; // xyz = emissive, w = anisotropy
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

// --- CSG cutters
//
// A cutter's operation and the name of the document node it came from. The
// name is a string rather than an id because it is authored data: the editor
// shows it and the scene serializer writes it back.
enum class CSGOperation : uint8_t { Difference = 0, Union = 1, Intersection = 2 };

struct CSGModifier {
    CSGOperation operation;
    std::string  operand_name;
};

// --- Descriptors the caller fills in

// Material recipe for RenderContext::CreateMaterial: pipeline-state flags
// plus the PBR factors and texture bindings of one scene material.
struct MaterialDesc {
    // Pipeline configuration
    bool doubleSided   = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    // PBR factors (using std::array eliminates memcpy)
    uint32_t             alphaMode   = 0;
    float                alphaCutoff = 0.5f;
    float                metallic    = 1.0f;
    float                roughness   = 1.0f;
    std::array<float, 4> baseColor   = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissive    = {0.0f, 0.0f, 0.0f, 1.0f};
    // See Material. Zero leaves an ordinary opaque/blend material.
    float                transmissionFactor = 0.0f;
    float                iridescenceFactor  = 0.0f;
    float                filmThicknessNm    = 0.0f;
    float                filmThicknessMinNm = 0.0f;
    float                volumeThicknessM   = 0.0f;
    float                ior                = 1.5f;
    float                normalScale        = 1.0f;

    // Texture bindings
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
};

struct DrawParams {
    JPH::Mat44           transform        = JPH::Mat44::sIdentity();
    JPH::Mat44           prevTransform    = JPH::Mat44::sIdentity();
    float                cullRadius       = 1.0f;
    std::array<float, 3> localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t             jointOffset      = 0;
    uint32_t             morphOffset      = 0;
    uint32_t             activeMorphCount = 0;
    const float*         morphWeights     = nullptr;
    DrawFlags            flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    std::array<float, 4> colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f}; // alpha < 0 means disable override
    std::array<float, 4> emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f}; // alpha < 0 means disable override
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
    ZHLN::Array<CSGCutterParams> cutters; // Stably using your custom Array container
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

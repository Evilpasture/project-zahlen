// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/COMTypes.hpp
//
// The vocabulary the public API speaks. Everything here is standard layout
// and trivially copyable, so it travels as plain bytes in registers or via
// reflected ChainInNode / ChainOutNode headers and never asks the two sides to
// agree on a container layout, an allocator or an exception table.
//
// The engine's own fixed-layout value types (Extent2D, BufferID, Entity, ...)
// are reused rather than mirrored. Where a struct holds growing configuration
// or asset state, vtable methods accept ChainInNode* / ChainOutNode* so size
// and layout drift is caught at the boundary rather than assumed.

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::COM {

// --- Chainable GPU resource & procedural geometry descriptors ----------------

struct BufferCreateInfo {
    BufferUsage           usage  = BufferUsage::Storage;
    Span<const std::byte> data   = {};
    size_t                size   = 0;
    uint32_t              stride = 1;
};

struct TextureCreateInfo {
    StringView            name         = {};
    Span<const std::byte> pixels       = {};
    Extent2D              extent       = {};
    bool                  isSRGB       = true;
    bool                  generateMips = true;
    bool                  isHDR        = false;
};

struct PbrMaterialConfig {
    ZHLN::FVec4 baseColor         {1.0f, 1.0f, 1.0f, 1.0f};
    ZHLN::FVec4 emissive          {0.0f, 0.0f, 0.0f, 1.0f};
    float       metallic          = 0.0f;
    float       roughness         = 0.5f;
    float       normalScale       = 1.0f;
    float       occlusionStrength = 1.0f;
    float       alphaCutoff       = 0.5f;
    uint32_t    alphaMode         = 0;
    TextureID   albedoMap         = TextureID::Invalid;
    TextureID   normalMap         = TextureID::Invalid;
    TextureID   pbrMap            = TextureID::Invalid;
    TextureID   emissiveMap       = TextureID::Invalid;
    TextureID   occlusionMap      = TextureID::Invalid;
    bool        doubleSided       = false;
    bool        unlit             = false;
    bool        alphaBlend        = false;
};

struct MeshBuildOptions {
    bool buildBLAS = false;
};

// Raw vertex/index stream upload descriptor. Populate either `indices16` or
// `indices32` for indexed meshes; `positions` is required.
struct MeshStreamsDesc {
    Span<const VertexPosition>     positions     = {};
    Span<const VertexTangentFrame> tangentFrames = {};
    Span<const VertexSurface>      surfaces      = {};
    Span<const VertexSkin>         skin          = {};
    Span<const uint16_t>           indices16     = {};
    Span<const uint32_t>           indices32     = {};
};

struct BoxMeshDesc {
    ZHLN::FVec3 halfExtents {0.5f, 0.5f, 0.5f};
    ZHLN::FVec4 color       {0.8f, 0.4f, 0.2f, 1.0f};
};

struct PlaneMeshDesc {
    float       extent = 10.0f;
    ZHLN::FVec4 color  {0.6f, 0.6f, 0.6f, 1.0f};
};

struct SphereMeshDesc {
    float       radius = 0.5f;
    ZHLN::FVec4 color  {0.8f, 0.4f, 0.2f, 1.0f};
};

struct CylinderMeshDesc {
    float       radius = 0.5f;
    float       height = 1.0f;
    ZHLN::FVec4 color  {0.8f, 0.4f, 0.2f, 1.0f};
};

struct ConeMeshDesc {
    float       radius = 0.5f;
    float       height = 1.0f;
    ZHLN::FVec4 color  {0.8f, 0.4f, 0.2f, 1.0f};
};

struct TetrahedronMeshDesc {
    uint8_t reserved = 0;
};

// The engine vocabulary this boundary hands across, checked rather than
// assumed. A type that stops satisfying these rules breaks the boundary, and it
// should say so here and not in a consumer's crash report.
static_assert(std::is_standard_layout_v<Extent2D> && std::is_trivially_copyable_v<Extent2D>);
static_assert(std::is_standard_layout_v<ViewportRect> && std::is_trivially_copyable_v<ViewportRect>);
static_assert(std::is_standard_layout_v<Entity> && std::is_trivially_copyable_v<Entity>);
static_assert(std::is_standard_layout_v<BufferID> && std::is_trivially_copyable_v<BufferID>);
static_assert(std::is_standard_layout_v<TextureID> && std::is_trivially_copyable_v<TextureID>);
static_assert(std::is_standard_layout_v<BufferUsage> && std::is_trivially_copyable_v<BufferUsage>);
static_assert(std::is_standard_layout_v<Mesh> && std::is_trivially_copyable_v<Mesh>);
static_assert(std::is_standard_layout_v<Material> && std::is_trivially_copyable_v<Material>);
static_assert(std::is_standard_layout_v<MaterialDesc> && std::is_trivially_copyable_v<MaterialDesc>);
static_assert(std::is_standard_layout_v<VertexPosition> && std::is_trivially_copyable_v<VertexPosition>);
static_assert(std::is_standard_layout_v<VertexTangentFrame> && std::is_trivially_copyable_v<VertexTangentFrame>);
static_assert(std::is_standard_layout_v<VertexSurface> && std::is_trivially_copyable_v<VertexSurface>);
static_assert(std::is_standard_layout_v<VertexSkin> && std::is_trivially_copyable_v<VertexSkin>);

static_assert(std::is_standard_layout_v<BufferCreateInfo> && std::is_trivially_copyable_v<BufferCreateInfo>);
static_assert(std::is_standard_layout_v<TextureCreateInfo> && std::is_trivially_copyable_v<TextureCreateInfo>);
static_assert(std::is_standard_layout_v<PbrMaterialConfig> && std::is_trivially_copyable_v<PbrMaterialConfig>);
static_assert(std::is_standard_layout_v<MeshBuildOptions> && std::is_trivially_copyable_v<MeshBuildOptions>);
static_assert(std::is_standard_layout_v<MeshStreamsDesc> && std::is_trivially_copyable_v<MeshStreamsDesc>);
static_assert(std::is_standard_layout_v<BoxMeshDesc> && std::is_trivially_copyable_v<BoxMeshDesc>);
static_assert(std::is_standard_layout_v<PlaneMeshDesc> && std::is_trivially_copyable_v<PlaneMeshDesc>);
static_assert(std::is_standard_layout_v<SphereMeshDesc> && std::is_trivially_copyable_v<SphereMeshDesc>);
static_assert(std::is_standard_layout_v<CylinderMeshDesc> && std::is_trivially_copyable_v<CylinderMeshDesc>);
static_assert(std::is_standard_layout_v<ConeMeshDesc> && std::is_trivially_copyable_v<ConeMeshDesc>);
static_assert(std::is_standard_layout_v<TetrahedronMeshDesc> && std::is_trivially_copyable_v<TetrahedronMeshDesc>);

// Pipeline configuration crosses via ChainInNode / ChainOutNode, either as a
// whole GraphicsSettings or as individual sub-slices.
static_assert(std::is_standard_layout_v<GraphicsSettings> && std::is_trivially_copyable_v<GraphicsSettings>);
static_assert(std::is_standard_layout_v<GISettings> && std::is_trivially_copyable_v<GISettings>);
static_assert(std::is_standard_layout_v<ShadowSettings> && std::is_trivially_copyable_v<ShadowSettings>);
static_assert(std::is_standard_layout_v<RayTracingConfig> && std::is_trivially_copyable_v<RayTracingConfig>);
static_assert(std::is_standard_layout_v<EnvironmentSettings> && std::is_trivially_copyable_v<EnvironmentSettings>);
static_assert(std::is_standard_layout_v<AAState> && std::is_trivially_copyable_v<AAState>);

} // namespace ZHLN::COM

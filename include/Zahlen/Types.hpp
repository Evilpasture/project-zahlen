// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Core/Reflection.hpp>
#include <array>
#include <cstdint>
#include <string_view>

namespace ZHLN {

/// Compile-time marker on GPU layout structs nested under `GPUTypes`.
/// Walk a purpose group with `ForEachNestedType<GPUTypes::Frame>` (same
/// pattern as `Components`). llvm-p2996 does not expose these annotations
/// through namespace `members_of`.
struct EnableABI {};

// --- High-Level Persistent Asset Identifiers ---
using AssetID    = uint64_t;
using MaterialID = uint64_t;

inline constexpr AssetID    InvalidAssetID    = 0;
inline constexpr MaterialID InvalidMaterialID = 0;

constexpr AssetID HashAssetID(std::string_view name) noexcept {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (char c: name) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

enum class GameplayStatus : int8_t { OK = 0, RequestQuit = 1, RequestReload = 2, Error = -1 };

// --- Core Math/Spatial Types ---

struct Extent2D {
    uint32_t width, height;
};
struct Offset2D {
    int32_t x, y;
};

struct ScissorRect {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
};

// Semantic types to help the Renderer choose the right Vulkan Format
struct Packed1010102 {
    uint32_t data;
}; // Normals/Tangents
struct PackedHalf2 {
    uint32_t data;
}; // UVs (2x 16-bit floats)
struct PackedRGBA8 {
    uint32_t data;
}; // Color

struct VertexPosition {
    float position[3]; // 12B - Full precision
};

struct VertexAttributes {
    Packed1010102 normal;  // 4B  - 10-bit per axis
    Packed1010102 tangent; // 4B  - 10-bit + sign
    PackedHalf2   uv;      // 4B  - 16-bit UVs
    PackedRGBA8   color;   // 4B  - RGBA8
}; // 16B - Perfect alignment

struct VertexSkin {
    uint16_t    joints[4]; // 8B  - 16-bit Joint indices
    PackedRGBA8 weights;   // 4B  - 8-bit UNORM weights mapped to [0.0, 1.0]
}; // 12B

// Partitioning limits. These match the guaranteed VK_EXT_mesh_shader minimums
// (maxMeshOutputVertices >= 256, maxMeshOutputPrimitives >= 256) with plenty of
// headroom, and are also the meshoptimizer build parameters used by the cooker
// and the runtime glTF importer, so cooked and JIT meshlets stay identical.
inline constexpr uint32_t kMeshletMaxVertices  = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124; // multiple of 4 (meshoptimizer recommendation)
inline constexpr float    kMeshletConeWeight   = 0.5f;
// Meshlets handled by one task-shader workgroup (one payload slot each).
inline constexpr uint32_t kMeshletsPerTaskGroup = 32;
// Threads per mesh-shader workgroup (one vertex per thread, 2 prims per thread).
inline constexpr uint32_t kMeshShaderGroupSize = 64;
// NOLINTBEGIN(performance-enum-size)
enum class TextureHandle : uint64_t { Invalid = 0 };
enum class TerrainHandle : uint64_t { Invalid = 0 };
enum class AudioHandle : uint64_t { Invalid = 0 };
enum class SynthHandle : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)
namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
} // namespace SystemTextures

struct UIBatch {
    TextureHandle texture              = TextureHandle::Invalid;
    uint32_t      bindlessTextureIndex = 0; // Non-zero bypasses TextureManager lookup (ImGui/user bindless IDs)
    uint32_t      vertexStart          = 0;
    uint32_t      vertexCount = 0;
    bool          useScissor      = false;
    bool          isSDF           = false;
    bool          useTextureColor = false;
    ScissorRect   scissorRect     = {};
};

struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44 invTransform;
    JPH::Vec4  extentsAndType;   // xyz = extents, w = type (0=Box, 1=Sphere)
    JPH::Vec4  colorAndDensity;  // xyz = color, w = density
    JPH::Vec4  emissiveAndAniso; // xyz = emissive, w = anisotropy
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

// --- Opaque Resource Handles ---
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };

static_assert(sizeof(BufferHandle) == 8);
static_assert(sizeof(PipelineHandle) == 8);
static_assert(sizeof(ResourceGroupHandle) == 8);
static_assert(sizeof(TextureHandle) == 8);
static_assert(sizeof(TerrainHandle) == 8);

struct Mesh {
    using enum BufferHandle;
    BufferHandle posBuffer   = Invalid;
    BufferHandle attrBuffer  = Invalid;
    BufferHandle skinBuffer  = Invalid;
    BufferHandle indexBuffer = Invalid;
    uint32_t     vertexCount = 0;
    uint32_t     indexCount  = 0;

    // --- VK_EXT_mesh_shader meshlet streams ---
    // The raw position/attribute/index buffers above are deliberately kept:
    // ray tracing BLAS builds (ZHLN_CmdBuildBlas) and the legacy vertex
    // pipeline still consume them. Meshlets are an additional view of the
    // very same vertex pool.
    BufferHandle meshletBuffer       = Invalid; // GPUMeshlet[]
    BufferHandle meshletVertexBuffer = Invalid; // uint32_t[]
    BufferHandle meshletTriBuffer    = Invalid; // uint8_t[] (padded to 4B)
    uint32_t     meshletCount        = 0;
};

enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
    Area,
    Sun,
};
static_assert(sizeof(LightType) == sizeof(uint32_t));

enum class ParticleAlignment : uint32_t { CameraBillboard = 0, VelocityStretched = 1, GroundFlat = 2 };

// GPU layout structs, grouped by Slang module. `ForEachNestedType<GPUTypes>`
// yields the groups; `ForEachNestedType<GPUTypes::Frame>` yields the leaves.
struct GPUTypes {
    // instance_data.slang
    struct Instance {
        // 64-byte meshlet descriptor. basic_task / basic_mesh index it through
        // a raw BDA pointer, so this layout is the authoritative GPU type.
        struct alignas(16) GPUMeshlet {
            uint32_t vertexOffset;
            uint32_t triangleOffset;
            uint32_t vertexCount;
            uint32_t triangleCount;

            float sphereCenter[3];
            float sphereRadius;

            float    coneApex[3];
            float    coneAxis[3];
            float    coneCutoff;
            uint32_t _pad;
        };
        static_assert(sizeof(GPUMeshlet) == 64);
        static_assert(alignof(GPUMeshlet) == 16);

        struct alignas(16) InstanceData {
            JPH::Mat44 world;
            JPH::Mat44 prevWorld;
            uint64_t   posAddress;
            uint64_t   attrAddress;
            uint64_t   skinAddress;
            uint64_t   iboAddress;
            uint32_t   vertexCount;
            uint32_t   indexCount;
            uint32_t   texIndices0;
            uint32_t   texIndices1;
            float      cullRadius;
            float      metallicFactor;
            float      roughnessFactor;
            float      alphaCutoff;
            uint32_t   flags;
            uint32_t   jointOffset;
            uint32_t   morphOffset;
            uint32_t   activeMorphCount;
            alignas(16) std::array<float, 3> localCenter;
            uint32_t _paddingCenter;
            alignas(16) std::array<float, 4> morphWeights;
            alignas(16) std::array<float, 4> baseColorFactor;
            alignas(16) std::array<float, 4> emissiveFactor;

            uint64_t meshletAddress;
            uint64_t meshletVertexAddress;
            uint64_t meshletTriAddress;
            uint32_t meshletCount;
            uint32_t _paddingMeshlet;
        };
        static_assert(sizeof(InstanceData) == 304);
    };

    // uniforms.slang
    struct Frame {
        struct alignas(16) Light {
            float     position[3];
            LightType type;
            float     color[3];
            float     intensity;
            float     direction[3];
            float     range;

            float points[4][4];

            float    radius;
            float    innerConeCos;
            float    outerConeCos;
            uint32_t twoSided;
            int32_t  shadowLayer;

            alignas(16) float positionView[3];
        };
        static_assert(sizeof(Light) == 160);

        struct alignas(16) FrameUniforms {
            JPH::Mat44 viewProj;
            JPH::Mat44 unjitteredViewProj;
            JPH::Mat44 prevUnjitteredViewProj;

            JPH::Mat44 lightSpaceMatrices[4];

            JPH::Mat44 invViewProj;
            float      camPos[4];
            float      lightDir[4];
            uint32_t   lightCount;
            float      ambientExposure;
            float      shadowWidth;
            uint32_t   shadowResolution;
            JPH::Vec4  sh[9];

            JPH::Vec4 probeMin;
            JPH::Vec4 probeMax;
            JPH::Vec4 probePos;
            JPH::Vec4 jitterParams;
            int       enableRTR;
            float     sunSize;

            alignas(16) float cascadeSplits[4];
            int   numCascades;
            int   fullBright;
            float screenResolution[2];

            JPH::Vec4 skyZenith;
            JPH::Vec4 skyHorizon;
            JPH::Vec4 skyGround;

            JPH::Mat44 viewmodelViewProj;
            JPH::Mat44 invProj;
        };
        static_assert(sizeof(FrameUniforms) % 16 == 0);
    };

    // cluster_math.slang
    struct Cluster {
        struct ClusterBounds {
            JPH::Vec4 minPoint;
            JPH::Vec4 maxPoint;
        };
        struct ClusterVolume {
            uint32_t offset;
            uint32_t count;
        };
    };

    // particles.slang
    struct Particles {
        struct alignas(16) Particle {
            JPH::Vec4 position = JPH::Vec4::sZero();
            JPH::Vec4 velocity = JPH::Vec4::sZero();
            JPH::Vec4 color    = JPH::Vec4::sReplicate(1.0f);
            JPH::Vec4 params   = JPH::Vec4::sZero();
        };
        static_assert(sizeof(Particle) == 64);

        struct alignas(16) Particle3D {
            JPH::Vec4 position;
            JPH::Vec4 velocity;
            JPH::Quat rotation;
            JPH::Vec4 rotVel;
            JPH::Vec4 color;
            JPH::Vec4 params;
        };
        static_assert(sizeof(Particle3D) == 96);

        struct alignas(16) ParticleEmitterParams {
            std::array<float, 3> gravity = {0.0f, -9.81f, 0.0f};
            float                drag    = 0.2f;

            std::array<float, 3> turbulence     = {0.0f, 0.0f, 0.0f};
            float                turbulenceFreq = 0.1f;

            std::array<float, 3> spawnOrigin = {0.0f, 0.0f, 0.0f};
            float                spawnRadius = 0.0f;

            std::array<float, 3> spawnBoxExtent = {10.0f, 10.0f, 10.0f};
            float                loopBoundary   = 0.0f;

            std::array<float, 3> initVelMin  = {-1.0f, -1.0f, -1.0f};
            float                lifetimeMin = 1.0f;

            std::array<float, 3> initVelMax  = {1.0f, 1.0f, 1.0f};
            float                lifetimeMax = 3.0f;

            std::array<float, 4> startColor = {1.0f, 1.0f, 1.0f, 1.0f};
            std::array<float, 4> endColor   = {1.0f, 1.0f, 1.0f, 0.0f};

            std::array<float, 2> startSize = {0.1f, 0.1f};
            std::array<float, 2> endSize   = {0.0f, 0.0f};

            float             spinSpeed    = 0.0f;
            uint32_t          textureIndex = 1;
            ParticleAlignment alignment    = ParticleAlignment::CameraBillboard;
            uint32_t          blendMode    = 0;
        };
        static_assert(sizeof(ParticleEmitterParams) == 160);

        struct alignas(16) MeshParticleEmitterParams {
            std::array<float, 3> gravity = {0.0f, -9.81f, 0.0f};
            float                drag    = 0.2f;

            std::array<float, 3> turbulence     = {0.0f, 0.0f, 0.0f};
            float                turbulenceFreq = 0.1f;

            std::array<float, 3> spawnOrigin = {0.0f, 0.0f, 0.0f};
            float                spawnRadius = 0.0f;

            std::array<float, 3> spawnBoxExtent = {10.0f, 10.0f, 10.0f};
            float                loopBoundary   = 0.0f;

            std::array<float, 3> initVelMin  = {-5.0f, 0.0f, -5.0f};
            float                lifetimeMin = 1.0f;

            std::array<float, 3> initVelMax  = {5.0f, 10.0f, 5.0f};
            float                lifetimeMax = 3.0f;

            std::array<float, 3> rotVelMin = {-3.14f, -3.14f, -3.14f};
            float                scaleMin  = 0.1f;

            std::array<float, 3> rotVelMax = {3.14f, 3.14f, 3.14f};
            float                scaleMax  = 0.5f;

            std::array<float, 4> startColor = {1.0f, 1.0f, 1.0f, 1.0f};
            std::array<float, 4> endColor   = {1.0f, 1.0f, 1.0f, 1.0f};
        };
        static_assert(sizeof(MeshParticleEmitterParams) == 160);
    };

    // volumetric_* + push_layouts.slang fog/inject/temporal
    struct Volume {
        struct alignas(16) VolumetricFogPushConstants {
            float density;
            float heightFalloff;
            float heightOffset;
            float anisotropy;

            float scatteringColor[3];
            float noiseScale;

            float absorptionColor[3];
            float noiseSpeed;

            float emissiveColor[3];
            float noiseIntensity;

            uint32_t volumeCount;
            uint32_t enableNoise;
            uint32_t _pad0;
            uint32_t _pad1;
        };
        static_assert(sizeof(VolumetricFogPushConstants) == 80);

        struct alignas(16) VolumetricLightInjectPushConstants {
            float    scatteringIntensity;
            float    ambientIntensity;
            float    phaseAnisotropy;
            uint32_t enableShadows;
        };
        static_assert(sizeof(VolumetricLightInjectPushConstants) == 16);

        struct alignas(16) VolumetricTemporalPushConstants {
            float    temporalWeight;
            float    clampStrength;
            uint32_t resetHistory;
            uint32_t _pad;
        };
        static_assert(sizeof(VolumetricTemporalPushConstants) == 16);
    };

    // push_layouts.slang per-draw / UI
    struct Draw {
        struct ObjectConstants {
            uint32_t instanceId;
            uint32_t isShadowPass;
        };
        static_assert(sizeof(ObjectConstants) == 8);

        struct UIObjectConstants {
            JPH::Mat44 orthoMatrix;
            uint64_t   posAddress;
            uint64_t   attrAddress;
            uint32_t   albedoIdx;
            uint32_t   isSDF;
            uint32_t   useTextureColor;
        };
        static_assert(sizeof(UIObjectConstants) == 96);
    };
};

using GPUMeshlet                         = GPUTypes::Instance::GPUMeshlet;
using InstanceData                       = GPUTypes::Instance::InstanceData;
using Light                              = GPUTypes::Frame::Light;
using FrameUniforms                      = GPUTypes::Frame::FrameUniforms;
using ClusterBounds                      = GPUTypes::Cluster::ClusterBounds;
using ClusterVolume                      = GPUTypes::Cluster::ClusterVolume;
using Particle                           = GPUTypes::Particles::Particle;
using Particle3D                         = GPUTypes::Particles::Particle3D;
using ParticleEmitterParams              = GPUTypes::Particles::ParticleEmitterParams;
using MeshParticleEmitterParams          = GPUTypes::Particles::MeshParticleEmitterParams;
using VolumetricFogPushConstants         = GPUTypes::Volume::VolumetricFogPushConstants;
using VolumetricLightInjectPushConstants = GPUTypes::Volume::VolumetricLightInjectPushConstants;
using VolumetricTemporalPushConstants    = GPUTypes::Volume::VolumetricTemporalPushConstants;
using ObjectConstants                    = GPUTypes::Draw::ObjectConstants;
using UIObjectConstants                  = GPUTypes::Draw::UIObjectConstants;

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
};

struct GISettings {
    int   mode              = 1;
    float aoRadius          = 0.5f;
    float aoBias            = 0.05f;
    float aoPower           = 1.8f;
    float giIntensity       = 1.2f;
    int   giSamples         = 8;
    float vignetteIntensity = 1.1f;
    float vignettePower     = 1.5f;
    int   enableSSR         = 1;
    int   enableRTR         = 0;
};

enum class AAMode : uint32_t { None = 0, FXAA, MLAA, TAA, SMAA };

struct AAState {
    AAMode mode = AAMode::TAA;

    float    taaFeedback = 0.95f;
    float    jitterX     = 0.0f;
    float    jitterY     = 0.0f;
    float    prevJitterX = 0.0f;
    float    prevJitterY = 0.0f;
    uint32_t frameIndex  = 0;

    float    fxaaSubpix           = 0.75f;
    float    fxaaEdgeThreshold    = 0.166f;
    float    fxaaEdgeThresholdMin = 0.0833f;
    float    mlaaThreshold        = 0.1f;
    uint32_t mlaaMaxSearchSteps   = 16;
};

struct GlyphMetric {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

struct FontAtlas {
    TextureHandle texture = TextureHandle::Invalid;
    GlyphMetric   glyphs[96] {};
};

enum class CSGOperation : uint8_t { Difference = 0, Union = 1, Intersection = 2 };

struct CSGModifier {
    CSGOperation operation;
    std::string  operand_name;
};

template <typename T>
inline constexpr bool EnableEnumFlags = false;

template <typename T>
concept EnumFlag = std::is_enum_v<T> && EnableEnumFlags<T>;

template <EnumFlag T>
constexpr T operator|(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator&(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator^(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator~(T a) noexcept {
    return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a));
}

template <EnumFlag T>
constexpr T& operator|=(T& a, T b) noexcept {
    a = a | b;
    return a;
}

template <EnumFlag T>
constexpr T& operator&=(T& a, T b) noexcept {
    a = a & b;
    return a;
}

template <EnumFlag T>
constexpr T& operator^=(T& a, T b) noexcept {
    a = a ^ b;
    return a;
}

template <EnumFlag T>
constexpr bool operator==(T a, T b) noexcept {
    return static_cast<std::underlying_type_t<T>>(a) == static_cast<std::underlying_type_t<T>>(b);
}

template <EnumFlag T>
constexpr bool operator!=(T a, T b) noexcept {
    return !(a == b);
}

enum class DrawFlags : uint32_t {
    None            = 0,
    ExcludeFromTLAS = 1 << 0,
    Skinned         = 1 << 1,
    VisibleInMain   = 1 << 2,
    VisibleInShadow = 1 << 3,
    Hidden          = 1 << 4,
    Viewmodel       = 1 << 5,
};
} // namespace ZHLN

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;

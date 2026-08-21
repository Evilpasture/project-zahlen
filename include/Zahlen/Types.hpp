// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <array>
#include <cstdint>
#include <string_view>

namespace ZHLN {

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

struct alignas(16) InstanceData {
    JPH::Mat44 world;
    JPH::Mat44 prevWorld;
    uint64_t   posAddress;
    uint64_t   attrAddress;
    uint64_t   skinAddress;
    uint64_t   iboAddress;
    uint32_t   vertexCount;
    uint32_t   indexCount;
    uint32_t   texIndices0; // [albedo:16 | normal:16]
    uint32_t   texIndices1; // [pbr:16    | emissive:16]
    float      cullRadius;
    float      metallicFactor;
    float      roughnessFactor;
    float      alphaCutoff;
    uint32_t   flags; // [alphaMode:8 | isSkinned:8 | isViewmodel:8 | padding:8]
    uint32_t   jointOffset;
    uint32_t   morphOffset;
    uint32_t   activeMorphCount;
    alignas(16) std::array<float, 3> localCenter;
    uint32_t _paddingCenter;
    alignas(16) std::array<float, 4> morphWeights;
    alignas(16) std::array<float, 4> baseColorFactor;
    alignas(16) std::array<float, 4> emissiveFactor;
};

enum class TextureHandle : uint64_t { Invalid = 0 };
enum class TerrainHandle : uint64_t { Invalid = 0 };

namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
} // namespace SystemTextures

struct UIObjectConstants {
    JPH::Mat44 orthoMatrix;
    uint64_t   posAddress;
    uint64_t   attrAddress;
    uint32_t   albedoIdx;
    uint32_t   isSDF;
};

struct UIBatch {
    TextureHandle texture     = TextureHandle::Invalid;
    uint32_t      vertexStart = 0;
    uint32_t      vertexCount = 0;
    bool          useScissor  = false;
    bool          isSDF       = false;
    ScissorRect   scissorRect = {};
};

struct ClusterBounds {
    JPH::Vec4 minPoint;
    JPH::Vec4 maxPoint;
};
struct ClusterVolume {
    uint32_t offset;
    uint32_t count;
};

struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44 invTransform;
    JPH::Vec4  extentsAndType;   // xyz = extents, w = type (0=Box, 1=Sphere)
    JPH::Vec4  colorAndDensity;  // xyz = color, w = density
    JPH::Vec4  emissiveAndAniso; // xyz = emissive, w = anisotropy
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

struct alignas(16) VolumetricFogInjectPushConstants {
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
static_assert(sizeof(VolumetricFogInjectPushConstants) == 80);

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

struct ObjectConstants {
    uint32_t instanceId;
    uint32_t isShadowPass;
};
static_assert(sizeof(ObjectConstants) == 8);
static_assert(sizeof(InstanceData) == 272);

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
static_assert(sizeof(ParticleEmitterParams) == 160, "ParticleEmitterParams alignment mismatch!");

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

struct alignas(16) GPULight {
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
static_assert(sizeof(GPULight) == 160);

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
    float     zScale;
    float     zBias;
    float     sunSize;

    alignas(16) float cascadeSplits[4];
    int   numCascades;
    int   fullBright;
    float screenResolution[2];

    JPH::Vec4 skyZenith;
    JPH::Vec4 skyHorizon;
    JPH::Vec4 skyGround;

    JPH::Mat44 viewmodelViewProj;
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

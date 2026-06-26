// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <cstdint>

namespace ZHLN {

// --- Core Math/Spatial Types ---

struct Extent2D {
	uint32_t width, height;
};
struct Offset2D {
	int32_t x, y;
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
	PackedHalf2 uv;		   // 4B  - 16-bit UVs
	PackedRGBA8 color;	   // 4B  - RGBA8
}; // 16B - Perfect alignment

struct VertexSkin {
	uint16_t joints[4];	 // 8B  - 16-bit Joint indices
	PackedRGBA8 weights; // 4B  - 8-bit UNORM weights mapped to [0.0, 1.0]
}; // 12B

struct alignas(16) InstanceData {
	JPH::Mat44 world;
	JPH::Mat44 prevWorld;
	uint64_t posAddress;
	uint64_t attrAddress;
	uint64_t skinAddress;
	uint64_t iboAddress;
	uint32_t vertexCount;
	uint32_t indexCount;
	uint32_t texIndices0; // [albedo:16 | normal:16]
	uint32_t texIndices1; // [pbr:16    | emissive:16]
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint32_t flags; // [alphaMode:8 | isSkinned:8 | padding:16]
	uint32_t jointOffset;
	uint32_t morphOffset;
	uint32_t activeMorphCount;
	alignas(16) std::array<float, 3> localCenter;
	uint32_t _paddingCenter;
	alignas(16) std::array<float, 4> morphWeights;
	alignas(16) std::array<float, 4> baseColorFactor;
	alignas(16) std::array<float, 4> emissiveFactor;
};

struct UIObjectConstants {
	JPH::Mat44 orthoMatrix;
	uint64_t posAddress;
	uint64_t attrAddress;
	uint32_t albedoIdx;
	uint32_t padding;
};

struct ClusterBounds {
	JPH::Vec4 minPoint;
	JPH::Vec4 maxPoint;
};
struct ClusterVolume {
	uint32_t offset;
	uint32_t count;
};

struct ObjectConstants {
	uint32_t instanceId;
	uint32_t isShadowPass;
};
static_assert(sizeof(ObjectConstants) == 8, "ObjectConstants must match HLSL alignment.");
static_assert(sizeof(InstanceData) == 272, "InstanceData must match HLSL alignment.");
// --- Opaque Resource Handles ---
// These abstract away Vulkan objects completely.
// NOLINTBEGIN(performance-enum-size)
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };
// NOLINTEND(performance-enum-size)

static_assert(sizeof(BufferHandle) == 8, "BufferHandle must be 64 bits!");
static_assert(sizeof(PipelineHandle) == 8, "PipelineHandle must be 64 bits!");
static_assert(sizeof(ResourceGroupHandle) == 8, "ResourceGroupHandle must be 64 bits!");

struct Mesh {
	BufferHandle posBuffer = BufferHandle::Invalid;
	BufferHandle attrBuffer = BufferHandle::Invalid;
	BufferHandle skinBuffer = BufferHandle::Invalid;
	BufferHandle indexBuffer = BufferHandle::Invalid;
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
};

// Align structures to 16-byte boundaries to match HLSL std430 layout
struct alignas(16) GPULight {
	float position[3];
	uint32_t type; // 0 = Dir, 1 = Point, 2 = Spot, 3 = Area (LTC Quad)
	float color[3];
	float intensity;
	float direction[3];
	float range;

	// --- Area Light Specific (64 Bytes) ---
	float points[4][4]; // XYZ = World Space Vertices, W = Padding / Flags

	float radius;
	float innerConeCos;
	float outerConeCos;
	uint32_t twoSided;	 // 0 = Single-Sided, 1 = Double-Sided Area Light
	int32_t shadowLayer; // -1 if no shadow, >= 0 for Atlas layer index
	float pad[3];
};
static_assert(sizeof(GPULight) == 144);

struct alignas(16) FrameUniforms {
	JPH::Mat44 viewProj;
	JPH::Mat44 unjitteredViewProj;
	JPH::Mat44 prevUnjitteredViewProj;
	JPH::Mat44 lightSpaceMatrix;
	JPH::Mat44 invViewProj;
	float camPos[4];
	float lightDir[4];
	uint32_t lightCount;
	float ambientExposure; // <-- Named explicitly (4 bytes)
	float shadowWidth;
	uint32_t shadowResolution;
	JPH::Vec4 sh[9]; // 9 Spherical Harmonic Coefficients

	// --- ADD PROBE PARAMETERS (3 * 16 = 48 bytes) ---
	JPH::Vec4 probeMin;		// XYZ: bounding box min, W: useLocalProbe flag (0.0 or 1.0)
	JPH::Vec4 probeMax;		// XYZ: bounding box max, W: unused
	JPH::Vec4 probePos;		// XYZ: probe capture position, W: unused
	JPH::Vec4 jitterParams; // x: currentX, y: currentY, z: prevX, w: prevY
	int enableRTR;
	float zScale;
	float zBias;
	int _padding_rtr;
};

// Material handle representation
struct Material {
	PipelineHandle pipeline = PipelineHandle::Invalid;
	ResourceGroupHandle resourceGroup = ResourceGroupHandle::Invalid;
	BufferHandle constantBuffer = BufferHandle::Invalid;
	uint32_t albedoIndex = 1;	// Default to Solid White (Index 1)
	uint32_t normalIndex = 2;	// Default to Flat Normal Map (Index 2)
	uint32_t pbrIndex = 0;		// Default to Solid Black (Index 0)
	uint32_t emissiveIndex = 1; // Default to Solid White (Index 1)
	float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float emissiveFactor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	float metallicFactor = 1.0f;
	float roughnessFactor = 1.0f;
	float alphaCutoff = 0.5f;
	uint32_t alphaMode = 0; // 0=Opaque, 1=Mask, 2=Blend
};

struct GISettings {
	int mode = 1; // 0 = Off, 1 = SSAO, 2 = SSGI
	float aoRadius = 0.5f;
	float aoBias = 0.05f;
	float aoPower = 1.8f;
	float giIntensity = 1.2f;
	int giSamples = 8;
	float vignetteIntensity = 1.1f; // 0.0f is completely off
	float vignettePower = 1.5f;		// Controls softness falloff
	int enableSSR = 1;
	int enableRTR = 0;
};
// NOLINTNEXTLINE(performance-enum-size)
enum class AAMode : uint32_t { None = 0, FXAA = 1, TAA = 2, SMAA = 3 };

struct AAState {
	AAMode mode = AAMode::TAA;

	// TAA Options
	float taaFeedback = 0.95f; // 95% History, 5% Current Frame
	float jitterX = 0.0f;
	float jitterY = 0.0f;
	float prevJitterX = 0.0f;
	float prevJitterY = 0.0f;
	uint32_t frameIndex = 0; // Drives the Halton Jitter sequence

	// FXAA Options
	float fxaaSubpix = 0.75f;
	float fxaaEdgeThreshold = 0.166f;
	float fxaaEdgeThresholdMin = 0.0833f;
};

template <typename T> inline constexpr bool EnableEnumFlags = false;

template <typename T>
concept EnumFlag = std::is_enum_v<T> && EnableEnumFlags<T>;

template <EnumFlag T> constexpr T operator|(T a, T b) noexcept {
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) |
						  static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T> constexpr T operator&(T a, T b) noexcept {
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) &
						  static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T> constexpr T operator^(T a, T b) noexcept {
	return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^
						  static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T> constexpr T operator~(T a) noexcept {
	return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a));
}

template <EnumFlag T> constexpr T& operator|=(T& a, T b) noexcept {
	a = a | b;
	return a;
}

template <EnumFlag T> constexpr T& operator&=(T& a, T b) noexcept {
	a = a & b;
	return a;
}

template <EnumFlag T> constexpr T& operator^=(T& a, T b) noexcept {
	a = a ^ b;
	return a;
}

// NOLINTNEXTLINE(performance-enum-size)
enum class DrawFlags : uint32_t {
	None = 0,
	ExcludeFromTLAS = 1 << 0, // Generic raytracing exclusion
	Skinned = 1 << 1,		  // Tells the renderer that this draw is skin-weighted
	VisibleInMain = 1 << 2,
	VisibleInShadow = 1 << 3,
};
} // namespace ZHLN

template <> inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;

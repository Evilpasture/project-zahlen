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

struct Vertex {
	float position[3];	   // 12B - Full precision
	Packed1010102 normal;  // 4B  - 10-bit per axis
	Packed1010102 tangent; // 4B  - 10-bit + sign
	PackedHalf2 uv;		   // 4B  - 16-bit UVs
	PackedRGBA8 color;	   // 4B  - RGBA8
	uint16_t joints[4];	   // 8B  - 16-bit Joint indices (Vulkan VK_FORMAT_R16G16B16A16_UINT)
	float weights[4];	   // 16B - 32-bit float weights (Vulkan VK_FORMAT_R32G32B32A32_SFLOAT)
	uint32_t _padding[3];  // 12B - Pad to exactly 64 bytes
};

static_assert(sizeof(Vertex) == 64, "Vertex must be exactly 64 bytes!");

struct alignas(16) InstanceData {
	JPH::Mat44 world;
	JPH::Mat44 prevWorld;
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t pbrIndex;
	uint32_t emissiveIndex;
	uint32_t vertexCount; // Kept (Required for morph targets!)
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint32_t alphaMode;
	uint32_t jointOffset;
	uint32_t isSkinned;
	uint32_t morphOffset;
	uint32_t activeMorphCount;
	uint32_t indexCount; // Added: Fits into padding
	uint32_t _pad;		 // Added: Aligns morphWeights to 16 bytes

	alignas(16) float morphWeights[4];
	alignas(16) float baseColorFactor[4];
};

struct FrameConstants {
	JPH::Mat44 transform;
	JPH::Mat44 prevTransform;
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t pbrIndex;
	uint32_t emissiveIndex;
	uint32_t isShadowPass;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint32_t alphaMode;
	uint32_t jointOffset;
	uint32_t isSkinned;
	uint32_t vertexCount; // Kept (Required for morph targets!)
	uint32_t morphOffset;
	uint32_t activeMorphCount;
	uint32_t indexCount; // Added: Fits into padding
	uint32_t _pad;		 // Added: Aligns morphWeights to 16 bytes

	alignas(16) float morphWeights[4];
	alignas(16) float baseColorFactor[4];
};
static_assert(sizeof(InstanceData) == 224, "InstanceData must match HLSL alignment.");
static_assert(sizeof(FrameConstants) == 224, "FrameConstants must match HLSL alignment.");
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
	BufferHandle vertexBuffer = BufferHandle::Invalid;
	BufferHandle indexBuffer = BufferHandle::Invalid; // Added
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0; // Added
};

// Align structures to 16-byte boundaries to match HLSL std430 layout
struct alignas(16) GPULight {
	float position[3];
	uint32_t type; // 0 = Directional, 1 = Point, 2 = Spot
	float color[3];
	float intensity;
	float direction[3];
	float range;
	float innerConeCos;
	float outerConeCos;
	float _padding[2];
};

struct alignas(16) FrameUniforms {
	JPH::Mat44 viewProj;
	JPH::Mat44 unjitteredViewProj;
	JPH::Mat44 prevUnjitteredViewProj;
	JPH::Mat44 lightSpaceMatrix;
	float camPos[4];
	float lightDir[4];
	uint32_t lightCount;
	float _padding[3];
};

struct ObjectConstants {
	JPH::Mat44 world;
	JPH::Mat44 prevWorld;
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t pbrIndex;
	uint32_t emissiveIndex;
};

// Material handle representation
struct Material {
	PipelineHandle pipeline = PipelineHandle::Invalid;
	ResourceGroupHandle resourceGroup = ResourceGroupHandle::Invalid;
	BufferHandle constantBuffer = BufferHandle::Invalid;
	uint32_t albedoIndex = 1;	// Default to Solid White (Index 1)
	uint32_t normalIndex = 2;	// Default to Flat Normal Map (Index 2)
	uint32_t pbrIndex = 0;		// Default to Solid Black (Index 0)
	uint32_t emissiveIndex = 0; // Default to Solid Black (Index 0)
	float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float metallicFactor = 1.0f;
	float roughnessFactor = 1.0f;
	float alphaCutoff = 0.5f;
	uint32_t alphaMode = 0; // 0=Opaque, 1=Mask, 2=Blend
};
} // namespace ZHLN

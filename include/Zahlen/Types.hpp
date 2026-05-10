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
	uint32_t _padding;	   // 4B  - Power of two alignment
};

static_assert(sizeof(Vertex) == 32, "Vertex must be exactly 32 bytes!");

struct FrameConstants {
	JPH::Mat44 transform;
	uint32_t textureIndex;
};

// --- Opaque Resource Handles ---
// These abstract away Vulkan/LLGL objects completely.

enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };

struct Mesh {
	BufferHandle vertexBuffer = BufferHandle::Invalid;
	uint32_t vertexCount = 0;
};

struct Material {
	PipelineHandle pipeline = PipelineHandle::Invalid;
	ResourceGroupHandle resourceGroup = ResourceGroupHandle::Invalid;
	BufferHandle constantBuffer = BufferHandle::Invalid;
	uint32_t textureIndex = 0;
};

} // namespace ZHLN
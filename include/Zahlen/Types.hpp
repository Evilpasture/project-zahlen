#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <array>
#include <cstdint>

namespace ZHLN {

struct Extent2D { uint32_t width, height; };
struct Offset2D { int32_t x, y; };

// Standard Layout Vertex for PBR
struct Vertex {
	std::array<float, 3> position;
	std::array<float, 3> normal;
	std::array<float, 4> tangent;
	std::array<float, 2> uv0;
	std::array<float, 2> uv1;
};

// Opaque Resource Handles
enum class BufferHandle : uint64_t { Invalid = 0 };

// Upgraded Mesh requires an Index Buffer
struct Mesh {
	BufferHandle vertexBuffer = BufferHandle::Invalid;
	BufferHandle indexBuffer = BufferHandle::Invalid;
	uint32_t indexCount = 0;
	uint32_t firstIndex = 0;
};

// Upgraded Material uses Bindless Texture Indices
struct Material {
	uint32_t albedoIdx = 0;
	uint32_t normalIdx = 0;
	uint32_t pbrIdx = 0;
	uint32_t emissiveIdx = 0;
	uint32_t lightmapIdx = 0;
};

} // namespace ZHLN
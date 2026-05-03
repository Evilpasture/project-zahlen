#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <cstdint>

namespace ZHLN {

// --- Core Math/Spatial Types ---

struct Extent2D { uint32_t width, height; };
struct Offset2D { int32_t x, y; };

struct Vertex {
	JPH::Vec3 position;
	JPH::Vec4 color;
};

struct FrameConstants {
	JPH::Mat44 transform;
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
};

} // namespace ZHLN
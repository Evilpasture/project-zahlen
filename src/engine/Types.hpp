#pragma once
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

// --- Core Data Layouts ---

struct Vertex {
	JPH::Vec3 position;
	JPH::Vec4 color;
};

struct FrameConstants {
	JPH::Mat44 transform;
};

// --- Resource Management ---

struct LLGLDeleter {
	LLGL::RenderSystem* system = nullptr;
	template <typename T> void operator()(T* resource) const {
		if (system && resource)
			system->Release(*resource);
	}
};

template <typename T> using LLGLPtr = std::unique_ptr<T, LLGLDeleter>;

using BufferPtr = LLGLPtr<LLGL::Buffer>;
using PipelinePtr = LLGLPtr<LLGL::PipelineState>;
using LayoutPtr = LLGLPtr<LLGL::PipelineLayout>;
using HeapPtr = LLGLPtr<LLGL::ResourceHeap>;

struct Mesh {
	BufferPtr vertexBuffer;
	uint32_t vertexCount = 0;
};

struct Material {
	PipelinePtr pipeline;
	LayoutPtr layout;
	HeapPtr resourceHeap;
	BufferPtr constantBuffer;
};

} // namespace ZHLN
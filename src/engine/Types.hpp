#pragma once
#include <LLGL/LLGL.h>

namespace ZHLN {

struct Mesh {
	LLGL::Buffer* vertexBuffer = nullptr;
	uint32_t vertexCount = 0;
};

struct Material {
	LLGL::PipelineState* pipeline = nullptr;
	LLGL::PipelineLayout* layout = nullptr;
	LLGL::ResourceHeap* resourceHeap = nullptr;
	LLGL::Buffer* constantBuffer = nullptr;
};

} // namespace ZHLN
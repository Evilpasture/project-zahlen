#pragma once
#include "engine/detail/String.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h> // New: For 4x4 Matrices
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

struct Vertex {
	JPH::Vec3 position;
	JPH::Vec4 color;
};

// New: CPU-side representation of the shader's constant buffer
struct FrameConstants {
	JPH::Mat44 transform;
};

class Renderer {
  public:
	Renderer(const String32& preferredAPI, uint32_t width, uint32_t height);
	~Renderer();

	bool IsRunning() const;
	void ProcessEvents();

	void BeginFrame();
	void Clear(const JPH::Vec4& color);

	// Updated: Now takes a transform matrix
	void DrawTriangle(const JPH::Mat44& transform);
	void EndFrame();

	enum class ColorComponent : size_t { R = 0, G = 1, B = 2, A = 3 };

  private:
	LLGL::RenderSystemPtr _system;
	std::unique_ptr<LLGL::CommandBuffer> _cmdBuffer;
	std::shared_ptr<LLGL::Window> _window;

	LLGL::SwapChain* _swapChain = nullptr;
	LLGL::CommandQueue* _commandQueue = nullptr;

	// GPU Resources
	LLGL::Buffer* _vertexBuffer = nullptr;
	LLGL::PipelineState* _pipeline = nullptr;

	// New: GPU Constant Buffer Resources
	LLGL::Buffer* _constantBuffer = nullptr;
	LLGL::PipelineLayout* _pipelineLayout = nullptr;
	LLGL::ResourceHeap* _resourceHeap = nullptr;

	void CreatePipeline();
};

} // namespace ZHLN
#pragma once
#include "engine/Types.hpp"
#include "engine/detail/String.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

// Standard Engine Formats
struct Vertex {
	JPH::Vec3 position;
	JPH::Vec4 color;
};

struct FrameConstants {
	JPH::Mat44 transform;
};

// 1. The Global State Group (Hardware & OS)
class RenderContext {
  public:
	RenderContext(const String32& preferredAPI, uint32_t width, uint32_t height);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	RenderContext& operator=(const RenderContext&) = delete;

	bool IsRunning() const;
	void ProcessEvents();

	void BeginFrame();
	void EndFrame();

	// Expose for external resource creation
	LLGL::RenderSystem* GetSystem() const { return _system.get(); }
	LLGL::CommandBuffer* GetCommandBuffer() const { return _cmdBuffer.get(); }
	LLGL::SwapChain* GetSwapChain() const { return _swapChain; }

  private:
	LLGL::RenderSystemPtr _system;
	std::unique_ptr<LLGL::CommandBuffer> _cmdBuffer;
	std::shared_ptr<LLGL::Window> _window;

	LLGL::SwapChain* _swapChain = nullptr;
	LLGL::CommandQueue* _commandQueue = nullptr;
};

// 2. Procedural Renderer Service
namespace Renderer {
enum class ColorComponent : size_t { R = 0, G = 1, B = 2, A = 3 };

void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth = 1.0f);

// Fast memory upload
void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const void* data, size_t size);

template <typename T>
inline void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const T& data) {
	UpdateBuffer(ctx, buffer, static_cast<const void*>(&data), sizeof(T));
}

// NEW: High-level draw command
void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform);
} // namespace Renderer

} // namespace ZHLN
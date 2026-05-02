#pragma once
#include "engine/Types.hpp"
#include "engine/Window.hpp"
#include "engine/detail/String.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec4.h>
#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

class RenderContext {
  public:
	RenderContext(Window& window, const String32& preferredAPI);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	RenderContext& operator=(const RenderContext&) = delete;

	void BeginFrame();
	void EndFrame();

	LLGL::RenderSystem* GetSystem() const { return _system.get(); }
	LLGL::CommandBuffer* GetCommandBuffer() const { return _cmdBuffer.get(); }
	LLGL::SwapChain* GetSwapChain() const { return _swapChain; }

  private:
	LLGL::RenderSystemPtr _system;
	std::unique_ptr<LLGL::CommandBuffer> _cmdBuffer;
	LLGL::SwapChain* _swapChain = nullptr;
	LLGL::CommandQueue* _commandQueue = nullptr;
};

namespace Renderer {
enum class ColorComponent : size_t { R = 0, G = 1, B = 2, A = 3 };

void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth = 1.0f);
void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const void* data, size_t size);

template <typename T>
inline void UpdateBuffer(RenderContext& ctx, LLGL::Buffer* buffer, const T& data) {
	UpdateBuffer(ctx, buffer, static_cast<const void*>(&data), sizeof(T));
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform);
} // namespace Renderer

} // namespace ZHLN
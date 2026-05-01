#pragma once

#include "engine/detail/String.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec4.h>
#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

class Renderer {
  public:
	Renderer(const String32& preferredAPI, uint32_t width, uint32_t height);
	~Renderer();

	// No copying allowed (don't want to double-free hardware)
	Renderer(const Renderer&) = delete;
	Renderer& operator=(const Renderer&) = delete;

	bool IsRunning() const;
	void ProcessEvents();

	void BeginFrame();
	void Clear(const JPH::Vec4& color);
	void EndFrame();

  private:
	// Unique owners
	LLGL::RenderSystemPtr _system;
	std::unique_ptr<LLGL::CommandBuffer> _cmdBuffer;

	// Shared owners (Required by LLGL API for the SwapChain)
	std::shared_ptr<LLGL::Window> _window;
	LLGL::SwapChain* _swapChain = nullptr;		 // Owned by _system
	LLGL::CommandQueue* _commandQueue = nullptr; // Owned by _system
};

} // namespace ZHLN
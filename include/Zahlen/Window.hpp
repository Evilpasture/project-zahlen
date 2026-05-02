#pragma once
#include <Zahlen/detail/String.hpp>

#include <LLGL/LLGL.h>
#include <memory>

namespace ZHLN {

class Window {
  public:
	Window(const String32& title, uint32_t width, uint32_t height);
	~Window();

	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	bool IsRunning() const;
	void ProcessEvents();
	void Focus(); // Implemented in Window.mm

	// Accessors for the RenderContext
	[[nodiscard]] auto GetNative() const -> const std::shared_ptr<LLGL::Window>& { return _native; }
	[[nodiscard]] auto GetSize() const -> LLGL::Extent2D { return _native->GetSize(); }

  private:
	std::shared_ptr<LLGL::Window> _native;
};

} // namespace ZHLN
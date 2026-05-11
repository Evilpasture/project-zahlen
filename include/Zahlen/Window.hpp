#pragma once
#include <Zahlen/Types.hpp>
#include <detail/String.hpp>
#include <memory>

namespace ZHLN {

class InputContext;

class Window {
  public:
	Window(const String32& title, uint32_t width, uint32_t height, InputContext* input);
	~Window();

	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	bool IsRunning() const;
	void ProcessEvents();
	void Focus();

	Extent2D GetSize() const;

	struct Impl;
	Impl* GetImpl() const { return _impl.get(); }

	// Returns the underlying LLGL::Window* as a void* to keep headers clean
	void* GetNativeHandle() const;

  private:
	std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
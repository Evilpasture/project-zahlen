#pragma once
#include <Zahlen/Types.hpp>
#include <detail/String.hpp>
#include <Zahlen/Common.h>
#include <memory>

namespace ZHLN {

class InputContext;

class ZHLN_API Window {
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

	void Close();

  private:
	std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN

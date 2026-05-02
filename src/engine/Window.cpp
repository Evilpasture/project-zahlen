#include "engine/Window.hpp"

namespace ZHLN {

Window::Window(const String32& title, uint32_t width, uint32_t height) {
	LLGL::WindowDescriptor desc;
	desc.title = title.c_str();
	desc.position = {};
	desc.size = {width, height};
	desc.flags =
		LLGL::WindowFlags::Visible | LLGL::WindowFlags::Centered | LLGL::WindowFlags::Resizable;

	_native = LLGL::Window::Create(desc);
}

Window::~Window() = default;

bool Window::IsRunning() const {
	return _native && !_native->HasQuit();
}

void Window::ProcessEvents() {
	_native->ProcessEvents();
}

} // namespace ZHLN
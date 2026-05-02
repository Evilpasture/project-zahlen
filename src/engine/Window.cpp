#include <Zahlen/Window.hpp>

#if defined(_WIN32)
#include <LLGL/Platform/Win32/Win32NativeHandle.h>
#include <Windows.h>
#endif

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

#if !APPLE
void Window::Focus() {
	if (!_native)
		return;

#if defined(_WIN32)
	LLGL::NativeHandle handle;
	if (_native->GetNativeHandle(&handle, sizeof(handle)) && handle.window) {
		SetForegroundWindow(handle.window);
		SetFocus(handle.window);
	}
#else
	// Non-Windows platforms without an explicit platform implementation
	// may not support programmatic focus promotion.
#endif
}
#endif

} // namespace ZHLN
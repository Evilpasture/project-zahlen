#include <Zahlen/Window.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

void Window::Focus() {
#ifdef _WIN32
    // Fetch the native HWND from LLGL
    LLGL::WindowDescriptor desc;
    _native->GetNativeHandle(&desc, sizeof(desc));
    HWND hwnd = (HWND)desc.windowContext; 
    
    if (hwnd) {
        // Bring to front and grab input focus
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
#endif
}

} // namespace ZHLN
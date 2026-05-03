#include <Zahlen/Window.hpp>
#include <Zahlen/Input.hpp>
#include <LLGL/LLGL.h>
#include <Zahlen/Platform.hpp>

namespace ZHLN {

static KeyCode MapLLGLKey(LLGL::Key k) {
	switch(k) {
		case LLGL::Key::W: return KeyCode::W;
		case LLGL::Key::A: return KeyCode::A;
		case LLGL::Key::S: return KeyCode::S;
		case LLGL::Key::D: return KeyCode::D;
		case LLGL::Key::LShift: return KeyCode::LShift;
		case LLGL::Key::RButton: return KeyCode::RButton;
		default: return KeyCode::Unknown;
	}
}

class WindowEventListener : public LLGL::Window::EventListener {
	InputContext* _input;
public:
	WindowEventListener(InputContext* ctx) : _input(ctx) {}
	void OnKeyDown(LLGL::Window&, LLGL::Key key) override { _input->InjectKeyDown(MapLLGLKey(key)); }
	void OnKeyUp(LLGL::Window&, LLGL::Key key) override { _input->InjectKeyUp(MapLLGLKey(key)); }
	void OnLocalMotion(LLGL::Window&, const LLGL::Offset2D& pos) override { _input->InjectLocalMotion((float)pos.x, (float)pos.y); }
	void OnWheelMotion(LLGL::Window&, int delta) override { _input->InjectWheelMotion((float)delta); }
	void OnResize(LLGL::Window&, const LLGL::Extent2D& ext) override { _input->InjectResize({ext.width, ext.height}); }
};

struct Window::Impl {
	std::shared_ptr<LLGL::Window> native;
	std::shared_ptr<WindowEventListener> listener;
};

Window::Window(const String32& title, uint32_t width, uint32_t height, InputContext* input) 
	: _impl(std::make_unique<Impl>()) {
	
	LLGL::WindowDescriptor desc;
	desc.title = title.c_str();
	desc.size = {width, height};
	desc.flags = LLGL::WindowFlags::Visible | LLGL::WindowFlags::Centered | LLGL::WindowFlags::Resizable;

	_impl->native = LLGL::Window::Create(desc);
	
	if (input) {
		_impl->listener = std::make_shared<WindowEventListener>(input);
		_impl->native->AddEventListener(_impl->listener);
	}
}

Window::~Window() = default;

bool Window::IsRunning() const { return _impl->native && !_impl->native->HasQuit(); }
void Window::ProcessEvents() { _impl->native->ProcessEvents(); }
Extent2D Window::GetSize() const { 
	auto s = _impl->native->GetSize(); 
	return {s.width, s.height}; 
}

void Window::Focus() {
    Platform::FocusWindow(*this);
}

void* Window::GetNativeHandle() const {
    return _impl->native.get();
}

} // namespace ZHLN
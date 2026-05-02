#include <Zahlen/Input.hpp>

namespace ZHLN {

void InputContext::ResetDeltas() {
	_mouse.deltaX = 0;
	_mouse.deltaY = 0;
	_mouse.wheel = 0;
}

void InputContext::OnKeyDown(LLGL::Window& /*sender*/, LLGL::Key key) {
	_keys[static_cast<size_t>(key)] = true;
}

void InputContext::OnKeyUp(LLGL::Window& /*sender*/, LLGL::Key key) {
	_keys[static_cast<size_t>(key)] = false;
}

void InputContext::OnLocalMotion(LLGL::Window& /*sender*/, const LLGL::Offset2D& position) {
	float x = static_cast<float>(position.x);
	float y = static_cast<float>(position.y);

	_mouse.x = x;
	_mouse.y = y;

	if (_firstMouse) {
		_lastX = x;
		_lastY = y;
		_firstMouse = false;
	}

	_mouse.deltaX = x - _lastX;
	_mouse.deltaY = y - _lastY;
	_lastX = x;
	_lastY = y;
}

void InputContext::OnWheelMotion(LLGL::Window& /*sender*/, int delta) {
	_mouse.wheel = static_cast<float>(delta);
}

void InputContext::OnResize(LLGL::Window& /*sender*/, const LLGL::Extent2D& extent) {
    _newSize = extent;
    _needsResize = true;
}

} // namespace ZHLN
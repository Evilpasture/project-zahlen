#include <Zahlen/Input.hpp>

namespace ZHLN {

void InputContext::ResetDeltas() {
	_mouse.deltaX = 0;
	_mouse.deltaY = 0;
	_mouse.wheel = 0;
}

void InputContext::InjectKeyDown(KeyCode key) {
	if (key == KeyCode::Unknown)
		return; // Never track the "Unknown" bit
	_keys[static_cast<size_t>(key)] = true;
}

void InputContext::InjectKeyUp(KeyCode key) {
	_keys[static_cast<size_t>(key)] = false;
}

void InputContext::InjectLocalMotion(float x, float y) {
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

void InputContext::InjectWheelMotion(float delta) {
	_mouse.wheel = delta;
}

void InputContext::InjectResize(const Extent2D& extent) {
	_newSize = extent;
	_needsResize = true;
}

} // namespace ZHLN
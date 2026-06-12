// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Input.hpp>
#include <imgui.h> // Include ImGui locally to avoid header pollution

namespace ZHLN {

void InputContext::ResetDeltas() {
	_mouse.deltaX = 0;
	_mouse.deltaY = 0;
	_mouse.wheel = 0;
}

bool InputContext::IsKeyDown(KeyCode key) const noexcept {
	if (key == KeyCode::Unknown) {
		return false;
	}

	// If ImGui is capturing keyboard input (e.g. typing), block gameplay keys
	if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard) {
		return false;
	}

	return _keys[static_cast<size_t>(key)];
}

bool InputContext::IsMouseButtonDown(KeyCode key) const noexcept {
	if (key == KeyCode::Unknown) {
		return false;
	}

	// If ImGui is capturing mouse input (hovered/clicked on UI), block gameplay clicks
	if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
		return false;
	}

	return _keys[static_cast<size_t>(key)];
}

void InputContext::InjectKeyDown(KeyCode key) {
	if (key == KeyCode::Unknown) {
		return;
	}
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

// src/engine/Input.cpp
#include <Zahlen/Components.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <imgui.h>
namespace ZHLN {

namespace {
[[nodiscard]] Components::InputStateComponent* GetOrCreateInfo(ECS::Registry* reg) noexcept {
    if (reg == nullptr) {
        return nullptr;
    }
    auto entities = reg->GetEntitiesWith<Components::InputStateComponent>();
    if (!entities.empty()) {
        return reg->Get<Components::InputStateComponent>(entities[0]);
    }
    Entity e = reg->Create(Components::InputStateComponent {});
    return reg->Get<Components::InputStateComponent>(e);
}
} // namespace

void InputManager::InjectKeyDown(KeyCode key) noexcept {
    if (key == KeyCode::Unknown) {
        return;
    }
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->keys[static_cast<size_t>(key)] = true;
    }
}

void InputManager::InjectKeyUp(KeyCode key) noexcept {
    if (key == KeyCode::Unknown) {
        return;
    }
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->keys[static_cast<size_t>(key)] = false;
    }
}

void InputManager::InjectLocalMotion(float x, float y) noexcept {
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->mouseX = x;
        state->mouseY = y;

        if (state->firstMouse) {
            state->lastX      = x;
            state->lastY      = y;
            state->firstMouse = false;
        }

        state->mouseDeltaX = x - state->lastX;
        state->mouseDeltaY = y - state->lastY;
        state->lastX       = x;
        state->lastY       = y;
    }
}

void InputManager::InjectWheelMotion(float delta) noexcept {
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->mouseWheel = delta;
    }
}

void InputManager::InjectResize(const Extent2D& extent) noexcept {
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->newSize     = extent;
        state->needsResize = true;
    }
}

void InputManager::ResetDeltas() noexcept {
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->mouseDeltaX = 0.0f;
        state->mouseDeltaY = 0.0f;
        state->mouseWheel  = 0.0f;
    }
}

bool InputManager::IsKeyDown(KeyCode key) const noexcept {
    if (key == KeyCode::Unknown || _registry == nullptr) {
        return false;
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard) {
        return false;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr && state->keys[static_cast<size_t>(key)];
}

bool InputManager::IsMouseButtonDown(KeyCode key) const noexcept {
    if (key == KeyCode::Unknown || _registry == nullptr) {
        return false;
    }
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        return false;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr && state->keys[static_cast<size_t>(key)];
}

bool InputManager::NeedsResize() const noexcept {
    if (_registry == nullptr) {
        return false;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr && state->needsResize;
}

Extent2D InputManager::GetNewSize() const noexcept {
    if (_registry == nullptr) {
        return {};
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->newSize : Extent2D {};
}

void InputManager::ClearResizeFlag() noexcept {
    if (_registry == nullptr) {
        return;
    }
    if (auto* state = GetOrCreateInfo(_registry)) {
        state->needsResize = false;
    }
}

float InputManager::GetMouseX() const noexcept {
    if (_registry == nullptr) {
        return 0.0f;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->mouseX : 0.0f;
}

float InputManager::GetMouseY() const noexcept {
    if (_registry == nullptr) {
        return 0.0f;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->mouseY : 0.0f;
}

float InputManager::GetMouseDeltaX() const noexcept {
    if (_registry == nullptr) {
        return 0.0f;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->mouseDeltaX : 0.0f;
}

float InputManager::GetMouseDeltaY() const noexcept {
    if (_registry == nullptr) {
        return 0.0f;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->mouseDeltaY : 0.0f;
}

float InputManager::GetMouseWheel() const noexcept {
    if (_registry == nullptr) {
        return 0.0f;
    }
    const auto* state = GetOrCreateInfo(_registry);
    return state != nullptr ? state->mouseWheel : 0.0f;
}

} // namespace ZHLN

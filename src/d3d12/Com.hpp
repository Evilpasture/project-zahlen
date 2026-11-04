#pragma once

// Minimal COM ownership for the d3d12 backend: a unique-ownership pointer
// that Releases on destruction and nothing more. Hand-rolled instead of
// WRL::ComPtr so the backend has no framework dependency (and so this
// header parses against the WSL header stubs on non-Windows hosts). The
// shape mirrors the engine's other handle wrappers: raw access is
// explicit (Get/Attach/Detach), creation goes through AddressOf().

#include "Sdk.hpp"

#include <cstddef>
#include <utility>

namespace ZHLN::D3D12 {

template <typename T>
class ComPtr {
  public:
    constexpr ComPtr() noexcept = default;
    constexpr explicit ComPtr(T* ptr) noexcept: _ptr(ptr) {
    }
    ~ComPtr() noexcept {
        Reset();
    }

    ComPtr(const ComPtr&)                    = delete;
    auto operator=(const ComPtr&) -> ComPtr& = delete;

    constexpr ComPtr(ComPtr&& other) noexcept: _ptr(std::exchange(other._ptr, nullptr)) {
    }
    auto operator=(ComPtr&& other) noexcept -> ComPtr& {
        if (this != &other) {
            Reset();
            _ptr = std::exchange(other._ptr, nullptr);
        }
        return *this;
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> T* {
        return _ptr;
    }
    [[nodiscard]] constexpr auto operator->() const noexcept -> T* {
        return _ptr;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return _ptr != nullptr;
    }
    [[nodiscard]] constexpr auto operator==(std::nullptr_t) const noexcept -> bool {
        return _ptr == nullptr;
    }

    // Creation slot: releases any current object and hands out the raw
    // address for an SDK factory call to fill (CreateDevice(&ptr) style).
    [[nodiscard]] auto AddressOf() noexcept -> T** {
        Reset();
        return &_ptr;
    }

    void Attach(T* ptr) noexcept {
        Reset();
        _ptr = ptr;
    }
    [[nodiscard]] auto Detach() noexcept -> T* {
        return std::exchange(_ptr, nullptr);
    }
    void Reset() noexcept {
        if (_ptr != nullptr) {
            _ptr->Release();
            _ptr = nullptr;
        }
    }

    // QueryInterface into another interface of the same object
    // (ID3D12Device -> ID3D12Device10 and friends).
    template <typename U>
    [[nodiscard]] auto As() const noexcept -> ComPtr<U> {
        ComPtr<U> out;
        if (_ptr != nullptr) {
            _ptr->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.AddressOf()));
        }
        return out;
    }

  private:
    T* _ptr = nullptr;
};

} // namespace ZHLN::D3D12

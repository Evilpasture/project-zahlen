// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/Ref.hpp
//
// An owning reference, not a pointer alias. It increments and decrements the
// object's own refcount, so the count and the delete live in the binary that
// created the object. `Adopt` takes ownership of a reference QueryRaw already
// added; the explicit constructor adds one of its own.

#include <Zahlen/Core/COM/IObject.hpp>

#include <utility>

namespace ZHLN::COM {

template <typename T>
class Ref {
public:
    constexpr Ref() noexcept = default;

    explicit Ref(T* pointer) noexcept: m_ptr(pointer) {
        if (m_ptr != nullptr) m_ptr->AddRef();
    }

    ~Ref() noexcept {
        if (m_ptr != nullptr) m_ptr->Release();
    }

    Ref(const Ref& other) noexcept: m_ptr(other.m_ptr) {
        if (m_ptr != nullptr) m_ptr->AddRef();
    }

    Ref(Ref&& other) noexcept: m_ptr(std::exchange(other.m_ptr, nullptr)) {
    }

    Ref& operator=(const Ref& other) noexcept {
        if (this != &other) {
            if (m_ptr != nullptr) m_ptr->Release();
            m_ptr = other.m_ptr;
            if (m_ptr != nullptr) m_ptr->AddRef();
        }
        return *this;
    }

    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            if (m_ptr != nullptr) m_ptr->Release();
            m_ptr = std::exchange(other.m_ptr, nullptr);
        }
        return *this;
    }

    template <typename Target>
    [[nodiscard]] auto As() const noexcept -> Ref<Target> {
        if (m_ptr == nullptr) return {};
        return Ref<Target>::Adopt(m_ptr->template Query<Target>());
    }

    [[nodiscard]] static Ref Adopt(T* raw) noexcept {
        Ref adopted;
        adopted.m_ptr = raw;
        return adopted;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return m_ptr != nullptr;
    }

    [[nodiscard]] T* operator->() const noexcept {
        return m_ptr;
    }

    [[nodiscard]] T* Get() const noexcept {
        return m_ptr;
    }

private:
    T* m_ptr = nullptr;
};

} // namespace ZHLN::COM

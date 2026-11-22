// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// An owning reference, not a pointer alias: the count and the delete live in the
// binary that created the object, so a Ref is only valid against a COM object
// whose vtable is still alive.

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

    // Acquire before release, always. Two Refs can name the same object, and
    // `this != &other` does not catch that: releasing first can run the
    // destructor and leave AddRef to touch freed memory. Releasing last also
    // means a destructor that reaches back into this Ref sees a finished
    // object rather than a half-assigned one.
    Ref& operator=(const Ref& other) noexcept {
        if (other.m_ptr != nullptr) other.m_ptr->AddRef();
        T* previous = m_ptr;
        m_ptr       = other.m_ptr;
        if (previous != nullptr) previous->Release();
        return *this;
    }

    // A move consumes other's reference instead of adding one, so the
    // pointer is taken first and the old one released once this Ref is
    // already consistent. Self-move leaves the object empty but valid.
    Ref& operator=(Ref&& other) noexcept {
        if (this == &other) return *this;
        T* adopted  = std::exchange(other.m_ptr, nullptr);
        T* previous = m_ptr;
        m_ptr       = adopted;
        if (previous != nullptr) previous->Release();
        return *this;
    }

    // Interface cast (any_cast-style name; Query<T> carries the result):
    // empty Ref when the object does not expose Target.
    template <typename Target>
    [[nodiscard]] auto cast() const noexcept -> Ref<Target> {
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

    [[nodiscard]] T& operator*() const noexcept {
        return *m_ptr;
    }

    [[nodiscard]] T* Get() const noexcept {
        return m_ptr;
    }

private:
    T* m_ptr = nullptr;
};

} // namespace ZHLN::COM
